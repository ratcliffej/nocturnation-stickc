#include "lume_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_rect / pulse_enabled
#include "../dal/drivers/led_strip_driver.h"   // signal-state pixel-0 overlay
#include "output_bindings/output_binding_registry.h"
#include "output_bindings/pixmob_ir.h"
#include "output_bindings/lume_led_strip.h"
#include "output_bindings/lume_text.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>          // WiFi.macAddress() for the repeat-census uid
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;
using nocturnation::output_bindings::output_binding_registry;
using nocturnation::output_bindings::OutputBinding;
using nocturnation::output_bindings::OutputBindingContext;
using nocturnation::output_bindings::pixmob_ir_instance;
using nocturnation::output_bindings::pixmob_ir_context;
using nocturnation::output_bindings::lume_led_strip_instance;
using nocturnation::output_bindings::lume_led_strip_context;
using nocturnation::output_bindings::lume_text_instance;
using nocturnation::output_bindings::lume_text_context;

namespace {

// Pair each registered binding singleton with the framework-owned
// OutputBindingContext declared alongside its instance / property_bag
// accessors. Unknown bindings return nullptr and are skipped by enter().
OutputBindingContext* context_for(const OutputBinding* binding) {
    if (binding == pixmob_ir_instance())      return &pixmob_ir_context();
    if (binding == lume_led_strip_instance()) return &lume_led_strip_context();
    if (binding == lume_text_instance())      return &lume_text_context();
    return nullptr;
}

}  // namespace

void LumeMode::enter() {
    rx_count_         = 0;
    last_rx_ms_       = 0;
    last_source_id_   = 0;
    last_msg_type_    = 0xFF;
    radio_active_     = false;
    no_signal_        = false;
    fallback_active_  = false;
    fallback_faded_   = false;
    last_pip_draw_ms_ = 0;
    lock_acquired_ms_ = 0;
    tofu_.clear();

    // LumeMode is a persistent singleton (mode_machine.cpp): a Config
    // round-trip re-enters this instance with the previous session's
    // state still present. Clear the dedup + pending queues BEFORE the
    // radio's WiFi-task callback is re-armed, so a fresh session never
    // fires a stale queued frame.
    std::memset(dedup_ring_, 0, sizeof(dedup_ring_));
    dedup_head_         = 0;
    pending_light_      = false;
    pending_repeat_     = false;
    pending_repeat_len_ = 0;

    lume_channel_pref_  = persistence::load_lume_channel();
    lume_repeat_en_     = persistence::load_lume_repeat_enabled();
    lume_group_           = persistence::load_lume_group();
    quality_.reset();

    // Apply persisted strip settings via DAL (centralised so TestMode /
    // DirectorMode / DmxBridgeMode all pick them up too - otherwise
    // strip defaults to 100 % and browns the board on white pulses).
    DAL::apply_persisted_strip_settings();

    // Show priority (ch 11) on auto-scan; locked pref honours itself.
    current_listen_chan_  = (lume_channel_pref_ == 0)
                            ? 11
                            : lume_channel_pref_;
    last_chan_switch_ms_  = millis();

    // LCD is not a lighting surface in Lume mode - blank it and let
    // the status pip be the only system UI overlay. reset_pulse_rect
    // keeps LocalDriver's state consistent for any non-binding path
    // that ever fires a pulse on "local".
    dal::local_driver_instance()->reset_pulse_rect();

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    draw_status_pip();      // initial paint so the pip exists

    // Skip PixMobIrBinding on a host running an enabled LED strip:
    // IRsend::sendRaw is a ~30 ms cli/sei blocking bit-bang that
    // breaks inter-Lume sparkle unison. Operator wanting a
    // Stick-as-PixMob-satellite disables the strip via Config.
    const bool strip_active = hal::HAL::has(hal::Capability::AddressableLeds)
                              && persistence::load_strip_enabled();
    active_binding_count_ = 0;
    auto& reg = output_binding_registry();
    for (size_t i = 0; i < reg.count() && active_binding_count_ < kMaxActiveBindings; ++i) {
        OutputBinding* b = reg.at(i);
        if (!b) continue;
        OutputBindingContext* bctx = context_for(b);
        if (!bctx) continue;
        if (!b->required_capabilities().subset_of(bctx->host_caps())) {
            continue;
        }
        if (b == pixmob_ir_instance() && strip_active) {
            continue;
        }
        bctx->mark_entered(millis());
        b->enter(*bctx);
        active_bindings_[active_binding_count_++] = ActiveBinding{b, bctx};
    }

    if (auto* radio = hal::HAL::esp_now()) {
        radio->set_recv_callback([this](const hal::ESPNowMessage& m) {
            this->on_recv(m);
        });
        radio_active_ = radio->begin(current_listen_chan_);
#ifdef ARDUINO
        // Same census identity scheme as RepeaterMode so the Director
        // dedups a Lume-repeater and a headless repeater identically.
        {
            uint8_t mac[6] = {0};
            WiFi.macAddress(mac);
            census_uid_[0] = mac[3];
            census_uid_[1] = mac[4];
            census_uid_[2] = mac[5];
        }
#endif
#ifdef ARDUINO
        if (!radio_active_) {
            Serial.println("[espnow] lume begin() failed");
        } else {
            Serial.printf("[espnow] lume up: ch=%u (pref=%s, bindings=%u)\n",
                          (unsigned)current_listen_chan_,
                          lume_channel_pref_ == 0 ? "auto"
                          : lume_channel_pref_ == 1 ? "1 hobby"
                          : lume_channel_pref_ == 11 ? "11 show"
                          : "6 custom",
                          (unsigned)active_binding_count_);
        }
#endif
    }
}

void LumeMode::exit() {
    if (radio_active_) {
        if (auto* radio = hal::HAL::esp_now()) radio->end();
        radio_active_ = false;
    }
    // Reverse-order drain (scope-stack discipline).
    for (size_t i = active_binding_count_; i-- > 0;) {
        auto& slot = active_bindings_[i];
        if (slot.binding && slot.ctx) {
            slot.binding->exit(*slot.ctx);
        }
        slot = ActiveBinding{};
    }
    active_binding_count_ = 0;
    dal::local_driver_instance()->reset_pulse_rect();
    // Clear the status overlay so subsequent modes get a clean strip.
    if (hal::HAL::led_strip() != nullptr) {
        led_strip_driver_instance()->set_overlay_pixel_0(0, 0, 0, false);
    }
}

void LumeMode::loop_tick() {
    const uint32_t now = millis();

    // Pump every binding's tick() at main-loop cadence (~20 Hz) for
    // tick-driven animations (e.g. text scroll).
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (slot.binding && slot.ctx) {
            slot.binding->tick(*slot.ctx, now);
        }
    }

    // Drain the WiFi-task deferred queues on main task. See
    // docs/stickc-history.md.
    if (pending_light_) {
        pending_light_ = false;
        fan_out_light_pulse(pending_light_payload_);
    }

    // Drain the signal-recovered repaint BEFORE the relay drain (and
    // before draw_repeat_count) so the R counter lands on the cleared
    // screen rather than being wiped by the recovery clear.
    if (signal_recovered_needs_repaint_) {
        signal_recovered_needs_repaint_ = false;
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        // Reset the cache so the next draw_repeat_count paints; the
        // cleared screen would otherwise match the cached value.
        last_drawn_repeats_ = 0xFFFFFFFFu;
        draw_status_pip();
        last_draw_ms_ = now;
    }

    if (pending_repeat_) {
        pending_repeat_ = false;
        if (auto* radio = hal::HAL::esp_now()) {
            radio->send_broadcast(pending_repeat_buf_, pending_repeat_len_);
            // Visible relay diagnostic so operators can confirm the
            // path fired without USB-monitoring serial.
            ++repeats_emitted_;
            draw_repeat_count();
#ifdef ARDUINO
            Serial.printf("[espnow REPEAT hop=%u] ",
                          (unsigned)pending_repeat_buf_[transport::espnow::kHopCountOffset]);
            for (size_t i = 0; i < pending_repeat_len_ && i < 32; ++i) {
                Serial.printf("%02X ", pending_repeat_buf_[i]);
            }
            Serial.println();
#endif
        }
    }

    // Keep this Lume visible in the Director's census while relaying.
    emit_repeat_census(now);

    // Saturating subtract: last_rx_ms_ is written from the WiFi-task
    // callback and can briefly be 1-2 ms ahead of `now`. Without the
    // guard that turns into ~UINT32_MAX and trips a spurious NO SIGNAL.
    const uint32_t age_since_rx =
        (now >= last_rx_ms_) ? (now - last_rx_ms_) : 0;
    if (rx_count_ > 0 && !no_signal_ && age_since_rx > kNoSignalMs) {
        no_signal_ = true;
        last_chan_switch_ms_ = now;   // reset scan timer
#ifdef ARDUINO
        Serial.printf("[espnow] lume NO SIGNAL: %lu ms since last RX\n",
                      (unsigned long)age_since_rx);
#endif
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        draw_no_signal_body();
        draw_status_pip();      // pip paints last so it sits on top
        last_draw_ms_ = now;
        return;
    }

    // Fallback wash edges anchored to age_since_rx so each fires once
    // per silence episode; rx_count_ > 0 guard stops cold-boot scanning
    // from tripping the fallback before any Director has ever been seen.
    // Reset in on_recv when a frame returns.
    if (rx_count_ > 0) {
        if (!fallback_active_ && age_since_rx > kFallbackEnterMs) {
            fallback_active_ = true;
            emit_fallback_wash_start();
        }
        if (fallback_active_ && !fallback_faded_
            && age_since_rx > kFallbackFadeStartMs) {
            fallback_faded_ = true;
            emit_fallback_wash_fade();
        }
    }

    // Three-channel scan (spec §5.3). Runs on cold boot and after
    // kRescanMs of silence (longer than kNoSignalMs so a transient
    // outage doesn't force a channel hunt). Locked Lumes never scan.
    // Any inbound frame implicitly stops the scan by locking us to
    // whichever channel it arrived on.
    const bool should_rescan = (rx_count_ > 0)
                            && (age_since_rx > kRescanMs);
    const bool scanning      = (lume_channel_pref_ == 0)
                            && (rx_count_ == 0 || should_rescan);
    if (scanning && (now - last_chan_switch_ms_) >= kChannelDwellMs) {
        size_t idx = 0;
        for (size_t i = 0; i < kScanOrderCount; ++i) {
            if (kScanOrder[i] == current_listen_chan_) { idx = i; break; }
        }
        idx = (idx + 1) % kScanOrderCount;
        current_listen_chan_ = kScanOrder[idx];
        last_chan_switch_ms_ = now;
        // A Director on the new channel has its own source_id; clear
        // the TOFU lock so the first frame on the new channel admits.
        tofu_.clear();
        if (auto* radio = hal::HAL::esp_now()) {
            radio->set_channel(current_listen_chan_);
#ifdef ARDUINO
            Serial.printf("[espnow] lume scan -> ch=%u\n",
                          (unsigned)current_listen_chan_);
#endif
        }
    }

    // TOFU lock-expiry edge. Next admitted frame re-establishes the
    // lock - so at a multi-show venue, if Director A goes silent and
    // Director B keeps broadcasting, the Lume drifts to B within ~10 s.
    if (tofu_.tick(now)) {
#ifdef ARDUINO
        Serial.println("[espnow] lume TOFU lock expired; ready to relock");
#endif
    }

    if (now - last_pip_draw_ms_ > kPipRefreshMs) {
        draw_status_pip();
        last_pip_draw_ms_ = now;
    }

    // Refresh the NO SIGNAL age counter periodically (no pulses arrive
    // in this state so it stays visible).
    if (no_signal_ && now - last_draw_ms_ > 200) {
        draw_no_signal_body();
        last_draw_ms_ = now;
    }

    // LED-strip status overlay - display-less hosts (Atom Lite) use
    // pixel 0 as the signal indicator (no LCD pip to carry it).
    //   Searching     -> flashing green at 1 Hz, 50 % duty
    //   FreshlyLocked -> solid green for kFreshLockMs
    //   Active        -> overlay off; pixel 0 belongs to the wash
    if (hal::HAL::led_strip() != nullptr && hal::HAL::display() == nullptr) {
        auto* strip_drv = led_strip_driver_instance();
        if (flash_group_remaining_ > 0) {
            // Group-cycle confirmation: N white pulses on pixel 0.
            // Off half decrements the remaining counter, giving exactly
            // N on/off pairs regardless of signal state.
            if (now >= flash_next_edge_ms_) {
                flash_on_ = !flash_on_;
                strip_drv->set_overlay_pixel_0(
                    flash_on_ ? 192 : 0,
                    flash_on_ ? 192 : 0,
                    flash_on_ ? 192 : 0,
                    true);
                flash_next_edge_ms_ = now + kGroupFlashHalfPeriodMs;
                if (!flash_on_ && --flash_group_remaining_ == 0) {
                    strip_drv->set_overlay_pixel_0(0, 0, 0, false);
                }
            }
        } else if (no_signal_ || rx_count_ == 0) {
            const uint32_t phase = now % kIndicatorFlashPeriodMs;
            const bool     lit   = phase < (kIndicatorFlashPeriodMs / 2);
            strip_drv->set_overlay_pixel_0(0, lit ? 96 : 0, 0, true);
        } else if ((now - lock_acquired_ms_) < kFreshLockMs) {
            strip_drv->set_overlay_pixel_0(0, 160, 0, true);
        } else {
            strip_drv->set_overlay_pixel_0(0, 0, 0, false);
        }
    }
}

void LumeMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        ModeMachine::switch_to(ModeId::Menu);
        return;
    }

    // Btn1 LongPressed on display-less hosts cycles group 1->2->3->1.
    // (g % 3) + 1 also pulls values outside {1,2,3} back into the cycle.
    // Confirms visually with a pixel-0 flash sequence.
#if NOCT_LUME_GROUP_LONGPRESS_ENABLED
    if (ev.id == ButtonId::Btn1
        && ev.kind == ButtonEvent::LongPressed
        && hal::HAL::display() == nullptr
        && hal::HAL::led_strip() != nullptr) {
        lume_group_ = static_cast<uint8_t>((lume_group_ % 3) + 1);
        persistence::save_lume_group(lume_group_);
        flash_group_remaining_ = lume_group_;
        flash_next_edge_ms_    = millis();
        flash_on_              = false;
#ifdef ARDUINO
        Serial.printf("[lume] group -> %u (flash %u pulses)\n",
                      (unsigned)lume_group_,
                      (unsigned)flash_group_remaining_);
#endif
        return;
    }
#endif
}

bool LumeMode::seen_recently(uint16_t src, uint8_t seq) const {
    if (seq == 0) return false;
    for (size_t i = 0; i < kDedupRingSize; ++i) {
        if (dedup_ring_[i].source_id == src
         && dedup_ring_[i].sequence_number == seq) {
            return true;
        }
    }
    return false;
}

void LumeMode::mark_seen(uint16_t src, uint8_t seq) {
    if (seq == 0) return;
    dedup_ring_[dedup_head_] = DedupEntry{src, seq};
    dedup_head_ = (dedup_head_ + 1) % kDedupRingSize;
}

// -------------------------------------------------------------------------
// LIGHT_PULSE fan-out to active OutputBindings.
//
// Class+group filter per binding:
//   Class: target_class == 0 OR matches binding->device_class().
//   Group: target_group == 0 OR matches lume_group_ - LOCAL bindings
//     only. Relay bindings (PixMobIrBinding) bypass this because their
//     downstream protocol filters by group at the bracelet.
//
// can_render_in_callback bindings are dispatched from on_recv already
// (see fan_out_light_pulse_inline); skip them here to avoid double-fire.
// -------------------------------------------------------------------------

void LumeMode::fan_out_light_pulse(const transport::espnow::LightPulsePayload& p) {
    RgbPulseEvent ev{};
    ev.r       = p.r;
    ev.g       = p.g;
    ev.b       = p.b;
    ev.attack  = static_cast<pixmob::Time>(p.attack);
    ev.sustain = static_cast<pixmob::Time>(p.sustain);
    ev.release = static_cast<pixmob::Time>(p.release);
    ev.chance  = static_cast<pixmob::Chance>(p.chance);

    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->can_render_in_callback()) continue;

        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // v3: u16 -> u8 narrowing is safe here - after the filter above,
        // p.target_group is either 0 or equal to lume_group_ (which is u8).
        slot.ctx->set_current_target(p.target_class,
                                     static_cast<uint8_t>(p.target_group));
        slot.binding->on_light_pulse(*slot.ctx, ev);
    }
}

void LumeMode::fan_out_light_pulse_inline(
        const transport::espnow::LightPulsePayload& p) {
    // Called from on_recv (WiFi task) for can_render_in_callback
    // bindings so the pulse start-time stamps within µs of the broadcast
    // landing on the radio. See docs/stickc-history.md.
    RgbPulseEvent ev{};
    ev.r       = p.r;
    ev.g       = p.g;
    ev.b       = p.b;
    ev.attack  = static_cast<pixmob::Time>(p.attack);
    ev.sustain = static_cast<pixmob::Time>(p.sustain);
    ev.release = static_cast<pixmob::Time>(p.release);
    ev.chance  = static_cast<pixmob::Chance>(p.chance);

    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (!slot.binding->can_render_in_callback()) continue;

        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // v3: u16 -> u8 narrowing is safe here - after the filter above,
        // p.target_group is either 0 or equal to lume_group_ (which is u8).
        slot.ctx->set_current_target(p.target_class,
                                     static_cast<uint8_t>(p.target_group));
        slot.binding->on_light_pulse(*slot.ctx, ev);
    }
}

// WASH-family fan-out. Same class+group filter as LIGHT_PULSE plus a
// can_wash capability gate. Bindings own their own wash state. WashPulse
// is routed unconditionally; the binding drops silently if no active wash.

void LumeMode::fan_out_light_wash(const transport::espnow::LightWashPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (!slot.binding->capabilities().can_wash) continue;

        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // v3: u16 -> u8 narrowing is safe here - after the filter above,
        // p.target_group is either 0 or equal to lume_group_ (which is u8).
        slot.ctx->set_current_target(p.target_class,
                                     static_cast<uint8_t>(p.target_group));
        slot.binding->on_light_wash(*slot.ctx, p);
    }
}

void LumeMode::fan_out_light_wash_end(const transport::espnow::LightWashEndPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (!slot.binding->capabilities().can_wash) continue;

        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // v3: u16 -> u8 narrowing is safe here - after the filter above,
        // p.target_group is either 0 or equal to lume_group_ (which is u8).
        slot.ctx->set_current_target(p.target_class,
                                     static_cast<uint8_t>(p.target_group));
        slot.binding->on_light_wash_end(*slot.ctx, p.release_time);
    }
}

void LumeMode::fan_out_light_wash_pulse(const transport::espnow::LightWashPulsePayload& p) {
    RgbPulseEvent ev{};
    ev.r       = p.r;
    ev.g       = p.g;
    ev.b       = p.b;
    ev.attack  = static_cast<pixmob::Time>(p.attack);
    ev.sustain = static_cast<pixmob::Time>(p.sustain);
    ev.release = static_cast<pixmob::Time>(p.release);
    ev.chance  = static_cast<pixmob::Chance>(p.chance);

    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (!slot.binding->capabilities().can_wash) continue;

        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // v3: u16 -> u8 narrowing is safe here - after the filter above,
        // p.target_group is either 0 or equal to lume_group_ (which is u8).
        slot.ctx->set_current_target(p.target_class,
                                     static_cast<uint8_t>(p.target_group));
        slot.binding->on_light_wash_pulse(*slot.ctx, ev);
    }
}

// Local-only fallback wash: synthesise a LIGHT_WASH so every wash-capable
// binding picks it up exactly as if the Director had emitted it. Never
// broadcast onto the radio. target_class = target_group = 0 so the group
// filter admits it regardless of the Lume's configured group.
void LumeMode::emit_fallback_wash_start() {
    transport::espnow::LightWashPayload p{};
    p.target_class   = 0;
    p.target_group   = 0;
    p.r1 = kFallbackColourA[0];  p.g1 = kFallbackColourA[1];  p.b1 = kFallbackColourA[2];
    p.r2 = kFallbackColourB[0];  p.g2 = kFallbackColourB[1];  p.b2 = kFallbackColourB[2];
    p.attack         = kFallbackAttackTicks;
    p.release        = 50;                              // 5 s default if cancelled without END
    p.intensity      = kFallbackIntensity;
    p.cycle_ms       = kFallbackCyclePeriodMs;
    p.ttl_seconds    = 0;                               // infinite; ended explicitly
    p.pulse_response = 0;                               // suppress overlay during calm idle
    fan_out_light_wash(p);
#ifdef ARDUINO
    Serial.println("[espnow] lume FALLBACK wash start (blue/purple cycle)");
#endif
}

void LumeMode::emit_fallback_wash_fade() {
    transport::espnow::LightWashEndPayload e{};
    e.target_class = 0;
    e.target_group = 0;
    e.release_time = kFallbackFadeTicks;
    fan_out_light_wash_end(e);
#ifdef ARDUINO
    Serial.println("[espnow] lume FALLBACK fade-to-black begin");
#endif
}

// Short fade-out on signal recovery: clear the synthetic baseline
// before the returning Director's own traffic starts competing.
void LumeMode::emit_fallback_wash_recovery() {
    transport::espnow::LightWashEndPayload e{};
    e.target_class = 0;
    e.target_group = 0;
    e.release_time = kFallbackRecoveryTicks;
    fan_out_light_wash_end(e);
#ifdef ARDUINO
    Serial.println("[espnow] lume FALLBACK wash recovery (signal returned)");
#endif
}

// Display-family fan-out. No target_class on the wire (message type IS
// the class), so filter by device_class() == Display. Target group
// follows the local-binding rule (no relay concept for Display).
void LumeMode::fan_out_text_display(const transport::espnow::TextDisplayPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->device_class() != hal::DeviceClass::Display) continue;
        if (p.target_group != 0 && p.target_group != lume_group_) continue;
        slot.ctx->set_current_target(
            static_cast<uint8_t>(hal::DeviceClass::Display),
            static_cast<uint8_t>(p.target_group));   // v3: safe narrow post-filter
        slot.binding->on_text_display(*slot.ctx, p);
    }
}

void LumeMode::fan_out_bitmap_header(const transport::espnow::BitmapHeaderPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->device_class() != hal::DeviceClass::Display) continue;
        if (p.target_group != 0 && p.target_group != lume_group_) continue;
        slot.ctx->set_current_target(
            static_cast<uint8_t>(hal::DeviceClass::Display),
            static_cast<uint8_t>(p.target_group));   // v3: safe narrow post-filter
        slot.binding->on_bitmap_header(*slot.ctx, p);
    }
}

void LumeMode::fan_out_bitmap_plane(const transport::espnow::BitmapPlanePayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->device_class() != hal::DeviceClass::Display) continue;
        if (p.target_group != 0 && p.target_group != lume_group_) continue;
        slot.ctx->set_current_target(
            static_cast<uint8_t>(hal::DeviceClass::Display),
            static_cast<uint8_t>(p.target_group));   // v3: safe narrow post-filter
        slot.binding->on_bitmap_plane(*slot.ctx, p);
    }
}

void LumeMode::fan_out_clear_screen(const transport::espnow::ClearScreenPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->device_class() != hal::DeviceClass::Display) continue;
        if (p.target_group != 0 && p.target_group != lume_group_) continue;
        slot.ctx->set_current_target(
            static_cast<uint8_t>(hal::DeviceClass::Display),
            static_cast<uint8_t>(p.target_group));   // v3: safe narrow post-filter
        slot.binding->on_clear_screen(*slot.ctx, p);
    }
}

void LumeMode::on_recv(const hal::ESPNowMessage& m) {
    using namespace transport::espnow;

    // Malformed frames don't count as our Director being alive - drop
    // before any liveness or TOFU update.
    Header hdr{};
    if (decode_header(m.data, m.len, hdr) != DecodeResult::Ok) {
#ifdef ARDUINO
        Serial.printf("[espnow RX BAD HDR] len=%u: ",
                      (unsigned)m.len);
        for (size_t i = 0; i < m.len && i < 32; ++i) {
            Serial.printf("%02X ", m.data[i]);
        }
        Serial.println();
#endif
        return;
    }

    // Silently ignore repeater census (Director telemetry only) instead
    // of running it through TOFU which would log an RXdrop per second.
    if (hdr.message_type == MessageType::RepeaterHeartbeat) return;

    // Spec §4.3: drop before admission so a hop-mangled attacker frame
    // can't establish a false TOFU lock. Frames at exactly kMaxHopCount
    // still render but the relay gate below refuses to re-broadcast them.
    if (hdr.hop_count > kMaxHopCount) {
#ifdef ARDUINO
        Serial.printf("[espnow RX HOP DROP] hop=%u src=%02X seq=%u\n",
                      hdr.hop_count, hdr.source_id, hdr.sequence_number);
#endif
        return;
    }

    // TofuLock partitions us between multiple co-channel Directors:
    // first eligible frame locks the source_id, others are dropped.
    // Display broadcasts (source_id = 0xFF) admit once a session exists
    // without resetting liveness. See docs/stickc-history.md.
    const uint32_t admit_now_ms = millis();
    if (!tofu_.admit(hdr.message_type,
                     hdr.source_id,
                     current_listen_chan_,
                     admit_now_ms)) {
#ifdef ARDUINO
        Serial.printf("[RXdrop %02X src=%02X]\n",
                      static_cast<unsigned>(hdr.message_type),
                      static_cast<unsigned>(hdr.source_id));
#endif
        return;
    }

    // Includes duplicates - redundant retransmits refresh liveness too.
    rx_count_++;
    last_rx_ms_ = admit_now_ms;

    const bool was_no_signal = no_signal_;
    no_signal_ = false;

    if (fallback_active_) {
        fallback_active_ = false;
        fallback_faded_  = false;
        emit_fallback_wash_recovery();
    }

    // Stamp on the first frame ever and on the NO SIGNAL recovery edge.
    if (was_no_signal || rx_count_ == 1) {
        lock_acquired_ms_ = last_rx_ms_;
    }
    if (was_no_signal) {
#ifdef ARDUINO
        Serial.println("[espnow] lume SIGNAL RECOVERED");
#endif
        // Defer the LCD repaint - SPI from the WiFi task crashes the S3.
        signal_recovered_needs_repaint_ = true;
    }

    last_source_id_ = hdr.source_id;
    last_msg_type_  = static_cast<uint8_t>(hdr.message_type);

    // Duplicates from the Director's redundant TX (spec §4.3) drop here
    // to prevent double IR fires / double paints per beat.
    const bool is_dup = seen_recently(hdr.source_id, hdr.sequence_number);
    if (!is_dup) {
        mark_seen(hdr.source_id, hdr.sequence_number);
        // Quality counts unique frames only; duplicates would flatter
        // the signal beyond what's actually being delivered.
        quality_.note_frame(hdr.source_id,
                            hdr.sequence_number,
                            last_rx_ms_);
    }

#ifdef ARDUINO
    // Command-only log: every Serial byte adds WiFi-callback latency at
    // peak traffic. DUP marker retained (free, useful for retransmit
    // correlation).
    Serial.printf(is_dup ? "[RXD %02X]\n" : "[RX %02X]\n",
                  (unsigned)last_msg_type_);
#endif

    if (is_dup) return;

    // Director-clock offset tracking (Phase 1 of §4.3 tick anchor).
    // Post-dedup because duplicates carry the same tick as the original.
    // See docs/stickc-history.md for the 90/10 smoothing and TOFU-relock
    // reseed rationale.
    if (hdr.message_type == MessageType::Heartbeat
        && (m.len - kHeaderSize) >= kHeartbeatPayloadLen) {
        HeartbeatPayload hb{};
        if (decode_heartbeat(hdr, m.data + kHeaderSize,
                             m.len - kHeaderSize, hb)
            == DecodeResult::Ok) {
            if (director_offset_valid_
                && hdr.source_id != director_offset_source_id_) {
                director_offset_valid_ = false;
            }
            const int32_t raw_offset =
                static_cast<int32_t>(hb.tick)
              - static_cast<int32_t>(last_rx_ms_);
            if (!director_offset_valid_) {
                director_tick_offset_ms_    = raw_offset;
                director_offset_source_id_  = hdr.source_id;
                director_offset_valid_      = true;
            } else {
                director_tick_offset_ms_ =
                    (director_tick_offset_ms_ * 9 + raw_offset) / 10;
            }
        }
    }

    // Repeater relay per §4.3: rebroadcast each unique frame once with
    // hop_count incremented, preserving source_id + sequence_number.
    // Deferred to loop_tick (same WiFi-task safety as the IR path).
    if (lume_repeat_en_
        && hdr.hop_count < kMaxHopCount
        && m.len <= kRepeatBufSize) {
        std::memcpy(pending_repeat_buf_, m.data, m.len);
        // hop_count is at header byte 5 in v2 (pre-2026-06-28 code wrote
        // byte 3 and corrupted source_id). Use the transport helper so
        // the offset can't drift from the header layout again.
        transport::espnow::set_hop_count(pending_repeat_buf_, m.len,
                                          hdr.hop_count + 1);
        pending_repeat_len_    = m.len;
        pending_repeat_        = true;
    }

    if (hdr.message_type == MessageType::LightPulse
        && m.len == kHeaderSize + kLightPulsePayloadLen) {
        LightPulsePayload p{};
        if (decode_light_pulse(hdr, m.data + kHeaderSize,
                                 m.len - kHeaderSize, p)
            == DecodeResult::Ok) {
            // Stamp callback-safe renderers immediately (µs-synced across
            // the fleet); queue the blocking path for loop_tick.
            fan_out_light_pulse_inline(p);
            pending_light_payload_ = p;
            pending_light_ = true;
        }
    }

    // Capability-gated fan-out: bindings that declare can_wash = false
    // are silently filtered inside the fan-out helpers.
    if (hdr.message_type == MessageType::LightWash
        && m.len == kHeaderSize + kLightWashPayloadLen) {
        LightWashPayload p{};
        if (decode_light_wash(hdr, m.data + kHeaderSize,
                              m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_light_wash(p);
        }
    }
    if (hdr.message_type == MessageType::LightWashEnd
        && m.len == kHeaderSize + kLightWashEndPayloadLen) {
        LightWashEndPayload p{};
        if (decode_light_wash_end(hdr, m.data + kHeaderSize,
                                  m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_light_wash_end(p);
        }
    }
    if (hdr.message_type == MessageType::LightWashPulse
        && m.len == kHeaderSize + kLightWashPulsePayloadLen) {
        LightWashPulsePayload p{};
        if (decode_light_wash_pulse(hdr, m.data + kHeaderSize,
                                    m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_light_wash_pulse(p);
        }
    }

    // Quick-drop gate: message types mapped to a specific capability
    // the host lacks skip the decode entirely. Atom Lite (no Display)
    // pays only the capability lookup for every inbound TEXT/BITMAP.
    const hal::Capability req_cap =
        message_type_required_capability(hdr.message_type);
    if (req_cap != kNoSpecificCapability && !hal::HAL::has(req_cap)) {
        return;
    }

    // Full hdr/body would take ~22 ms of blocking Serial.printf per
    // frame (up to 192 bytes at 115200 baud) x3 retransmits -> ~66 ms
    // of WiFi-task block per lyric, overflowing the IDF receive queue.
    // The type-only [RX 09] line above is the diagnostic; correlate
    // content with the orchestrator's display-emit log.
    if (hdr.message_type == MessageType::TextDisplay
        && m.len >= kHeaderSize + kTextDisplayMinPayloadLen) {
        TextDisplayPayload p{};
        if (decode_text_display(hdr, m.data + kHeaderSize,
                                m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_text_display(p);
        }
    }
    if (hdr.message_type == MessageType::BitmapHeader
        && m.len == kHeaderSize + kBitmapHeaderPayloadLen) {
        BitmapHeaderPayload p{};
        if (decode_bitmap_header(hdr, m.data + kHeaderSize,
                                 m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_bitmap_header(p);
        }
    }
    if (hdr.message_type == MessageType::BitmapPlane
        && m.len >= kHeaderSize + kBitmapPlaneMinPayloadLen) {
        BitmapPlanePayload p{};
        if (decode_bitmap_plane(hdr, m.data + kHeaderSize,
                                m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_bitmap_plane(p);
        }
    }
    if (hdr.message_type == MessageType::ClearScreen
        && m.len == kHeaderSize + kClearScreenPayloadLen) {
        ClearScreenPayload p{};
        if (decode_clear_screen(hdr, m.data + kHeaderSize,
                                m.len - kHeaderSize, p) == DecodeResult::Ok) {
            fan_out_clear_screen(p);
        }
    }
}

// Frame-age proxy: cold-start fallback before the quality tracker has
// data, and the post-NO-SIGNAL killer regardless of history.
int LumeMode::signal_bars_from_age() const {
    if (rx_count_ == 0)              return 0;
    // Saturating subtract for the WiFi-task/main-task race.
    const uint32_t now = millis();
    const uint32_t age = (now >= last_rx_ms_) ? (now - last_rx_ms_) : 0;
    if (age < 500)                   return 4;
    if (age < 1000)                  return 3;
    if (age < 2000)                  return 2;
    if (age < kNoSignalMs)           return 1;
    return 0;
}

// Sequence-loss-rate is the primary metric; fall back to age proxy on
// cold start (not enough frames yet) and NO SIGNAL (age beats history).
int LumeMode::signal_bars() const {
    if (no_signal_ || rx_count_ == 0)            return 0;
    const int q = quality_.bars(millis());
    if (q < 0)                                    return signal_bars_from_age();
    const int a = signal_bars_from_age();
    return (q < a) ? q : a;
}

void LumeMode::draw_status_pip() {
    // Batched sprite: without begin_buffered_paint each fill_rect writes
    // to the panel independently and a scan-out crossing one shows tear
    // lines between pip elements.
    auto* ld = dal::local_driver_instance();
    const bool buffered =
        ld->begin_buffered_paint(kPipX, kPipY, kPipWidth, kPipHeight);

    // Opaque wipe first: a full-screen pulse may have painted any
    // colour under the pip since last refresh, so signal/battery
    // colours would otherwise composite against arbitrary backgrounds.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        kPipX, kPipY, kPipWidth, kPipHeight, BLACK});

    // 4 bar = green, 2-3 bar = amber, 0-1 bar = red.
    const int bars       = signal_bars();
    const uint16_t dot_c = (bars >= 4) ? GREEN
                         : (bars >= 2) ? YELLOW
                         :               RED;
    const int dot_x = kPipX + 2;
    const int dot_y = kPipY + 3;
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        dot_x, dot_y, 6, 6, dot_c});

    // Battery glyph: outline + tip + fill.
    const int batt_w = 14;             // body width
    const int batt_h = 8;
    const int batt_x = kPipX + kPipWidth - batt_w - 2;   // 2 px tip + edge
    const int batt_y = kPipY + 2;
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y, batt_w, 1, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y + batt_h - 1, batt_w, 1, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y, 1, batt_h, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x + batt_w - 1, batt_y, 1, batt_h, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x + batt_w, batt_y + 2, 2, batt_h - 4, WHITE});

    const int level = DAL::battery_level("local");
    if (level >= 0) {
        const int interior_w = batt_w - 2;
        int fill_w = (level * interior_w) / 100;
        if (fill_w < 0) fill_w = 0;
        if (fill_w > interior_w) fill_w = interior_w;
        const uint16_t fill_color = (level > 20) ? GREEN
                                   : (level > 5)  ? YELLOW
                                                  : RED;
        if (fill_w > 0) {
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                batt_x + 1, batt_y + 1, fill_w, batt_h - 2, fill_color});
        }
    }

    if (buffered) {
        ld->end_buffered_paint();
    }

    // Relay counter sits outside the pip buffer (separate screen region).
    draw_repeat_count();
}

void LumeMode::emit_repeat_census(uint32_t now) {
    // Gated on actually relaying (repeat mode on AND Director lock held).
    // Census header uses the broadcast source_id which TofuLock rejects
    // on every receiver, so it can't steal a lock or refresh liveness -
    // only the Director's census intake reads it (dedup key = payload uid).
    if (!lume_repeat_en_ || !radio_active_) return;
    if (!tofu_.is_locked() || no_signal_) return;
    if (now - last_census_ms_ < kCensusIntervalMs) return;
    last_census_ms_ = now;

    auto* radio = hal::HAL::esp_now();
    if (radio == nullptr) return;

    namespace te = transport::espnow;
    te::Header hdr{};
    hdr.source_id       = te::kBroadcastSourceId;
    hdr.sequence_number = 0;   // census is unsequenced
    hdr.hop_count       = 0;

    te::RepeaterHeartbeatPayload p{};
    p.uid[0]  = census_uid_[0];
    p.uid[1]  = census_uid_[1];
    p.uid[2]  = census_uid_[2];
    p.channel = current_listen_chan_;
    // millis() epoch is boot, so this is genuine device uptime.
    const uint32_t up = now / 1000u;
    p.uptime_s      = (up > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(up);
    p.relayed_count = repeats_emitted_;

    uint8_t buf[te::kHeaderSize + te::kRepeaterHeartbeatPayloadLen];
    const size_t n = te::encode_repeater_heartbeat(buf, sizeof(buf), hdr, p);
    if (n > 0) radio->send_broadcast(buf, n);
}

void LumeMode::draw_repeat_count() {
    if (!lume_repeat_en_) {
        // One-shot wipe on the toggle-OFF edge to erase any leftover
        // "R:NNN" from a previous session. Sentinel prevents repaint.
        if (last_drawn_repeats_ != 0xFFFFFFFEu) {
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                0, 120, 80, 12, BLACK});
            last_drawn_repeats_ = 0xFFFFFFFEu;
        }
        return;
    }
    if (repeats_emitted_ == last_drawn_repeats_) return;
    last_drawn_repeats_ = repeats_emitted_;

    // Wipe just the digit area so a shorter new count doesn't composite
    // against the previous longer one.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        0, 120, 80, 12, BLACK});

    char line[16];
    std::snprintf(line, sizeof(line), "R:%lu",
                  (unsigned long)repeats_emitted_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        2, 122, line, GREEN, BLACK, 1});
}

void LumeMode::draw_no_signal_body() {
    char line[40];

    // Centre "NO SIGNAL" horizontally under the pip: 9 chars * 18 px
    // = 162 px, (240-162)/2 = 39 px left margin.
    constexpr int kHeadlineX = (240 - 9 * 18) / 2;
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kHeadlineX, 20, "NO SIGNAL", RED, BLACK, 3});

    constexpr int kDiagX     = 10;
    constexpr int kDiagY     = 60;
    constexpr int kLineStep  = 14;

    std::snprintf(line, sizeof(line), "ch %u %s%s",
                  (unsigned)current_listen_chan_,
                  radio_active_ ? "listening" : "off",
                  lume_channel_pref_ == 0 ? " (scan)" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kDiagX, kDiagY, line, WHITE, BLACK, 1});

    std::snprintf(line, sizeof(line), "rx total: %lu",
                  (unsigned long)rx_count_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kDiagX, kDiagY + kLineStep, line, WHITE, BLACK, 1});

    if (rx_count_ > 0) {
        const uint32_t now = millis();
        const uint32_t age = (now >= last_rx_ms_) ? (now - last_rx_ms_) : 0;
        std::snprintf(line, sizeof(line), "last rx: %lu ms ago    ",
                      (unsigned long)age);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kDiagX, kDiagY + 2 * kLineStep, line, RED, BLACK, 1});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kDiagX, 122, "B-hold: menu", WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
