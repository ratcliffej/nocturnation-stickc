// LumeMode implementation.

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

// Per-binding context lookup. The framework owns one OutputBindingContext
// per registered binding singleton (declared alongside the binding's
// instance + property_bag accessors). The walk in enter() pairs each
// binding with its own context here. Falls back to nullptr for any
// binding that wasn't expected at registration time - those still get
// activated but with no per-binding context (their on_light_pulse
// has to operate context-free or skip work; the two we ship today
// always have a context).
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

    // LumeMode is a persistent static singleton (mode_machine.cpp), so
    // a Config round-trip re-enters this same instance with the
    // previous session's dedup ring and any staged-but-undrained
    // pending_light_/pending_repeat_ frame still present. Reset them
    // here - before radio->begin() below re-arms the WiFi-task recv
    // callback - so a fresh Lume session starts from a clean dedup
    // window and never fires a stale queued frame.
    std::memset(dedup_ring_, 0, sizeof(dedup_ring_));
    dedup_head_         = 0;
    pending_light_      = false;
    pending_repeat_     = false;
    pending_repeat_len_ = 0;

    // Load operator-configured preferences from NVS. Channel preference
    // picks hobby (1) / show (11) / advanced (6) / auto-scan (0) per
    // spec §4.5. The slv_ir_grp setting moved to PixMobIrBinding's
    // property bag in Block 9; ConfigMode > IR > Lume Group mutates
    // it there and on_light_pulse reads it inline.
    lume_channel_pref_  = persistence::load_lume_channel();
    lume_repeat_en_     = persistence::load_lume_repeat_enabled();
    lume_group_           = persistence::load_lume_group();
    quality_.reset();

    // Apply persisted LED-strip settings to the driver. Cheap no-op
    // on hosts without a strip (the driver still stores the values
    // even if no strip is wired). The driver's enabled() flag is
    // toggled via DAL so the rest of the dispatch path sees it.
    // Apply persisted strip settings (brightness percent, group size,
    // pixel count, enabled flag). Centralised in DAL so TestMode /
    // DirectorMode / DmxBridgeMode can call the same hook on enter()
    // and the strip respects the operator's Config-menu setting in
    // every mode, not just Lume. Pre-fix this block lived inline
    // here and the other modes silently ran the strip at default
    // 100 % brightness - brownout-on-white symptom for the Pulse /
    // Whiteout tests and the DMX raw-RGB path.
    DAL::apply_persisted_strip_settings();

    // Auto-scan starts on channel 11 (show priority) per spec §4.5.
    // Locked configs start on the configured channel.
    current_listen_chan_  = (lume_channel_pref_ == 0)
                            ? 11
                            : lume_channel_pref_;
    last_chan_switch_ms_  = millis();

    // Epic 13 B0: LCD is no longer a lighting surface in Lume mode.
    // Blank it on entry and let the status pip be the only system UI
    // overlay; future Epic 13 blocks land the new text + bitmap
    // bindings that paint content here. reset_pulse_rect keeps the
    // driver's pulse-rect state consistent if any non-binding path
    // ever fires a pulse on "local" (e.g. Director-side loopback when
    // this Stick is in Director mode rather than Lume).
    dal::local_driver_instance()->reset_pulse_rect();

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    draw_status_pip();      // initial paint so the pip exists

    // Activate output bindings whose required capabilities the host
    // supports. Walks the registry, gates each one on
    // required_capabilities().subset_of(host_caps), and calls
    // binding->enter(ctx) on each accepted entry. Post-Epic-12 ships
    // PixMobIrBinding + LumeLedStripBinding; Epic 13 adds text +
    // bitmap. kMaxActiveBindings (4) will need bumping when those land.
    // Epic 13: PixMob IR relay vs LedStrip mutex. On a host that has
    // both IRTx + LedStrip and the strip is actually enabled in Config,
    // skip activating PixMobIrBinding - IRsend::sendRaw is a ~30 ms
    // cli/sei blocking bit-bang that breaks inter-Lume sparkle unison.
    // Operator who explicitly wants Stick-as-PixMob-IR-satellite turns
    // the strip off via Config > LED Strip > Enable; PixMobIrBinding
    // then activates as normal on the next Lume re-entry.
    const bool strip_active = hal::HAL::has(hal::Capability::LedStrip)
                              && persistence::load_strip_enabled();
    active_binding_count_ = 0;
    auto& reg = output_binding_registry();
    for (size_t i = 0; i < reg.count() && active_binding_count_ < kMaxActiveBindings; ++i) {
        OutputBinding* b = reg.at(i);
        if (!b) continue;
        OutputBindingContext* bctx = context_for(b);
        if (!bctx) continue;     // unknown binding; skip until wiring lands
        if (!b->required_capabilities().subset_of(bctx->host_caps())) {
            continue;
        }
        // Strip / IR mutex gate (see comment above the loop).
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
    // Drain active bindings in reverse order so the last one entered
    // is the first to exit (mirrors a typical scope stack discipline).
    for (size_t i = active_binding_count_; i-- > 0;) {
        auto& slot = active_bindings_[i];
        if (slot.binding && slot.ctx) {
            slot.binding->exit(*slot.ctx);
        }
        slot = ActiveBinding{};
    }
    active_binding_count_ = 0;
    // Restore full-screen pulse rect for whichever mode comes next.
    dal::local_driver_instance()->reset_pulse_rect();
    // Release the LED-strip status overlay so subsequent modes get a
    // clean strip render. Cheap no-op on hosts without a strip.
    if (hal::HAL::led_strip() != nullptr) {
        led_strip_driver_instance()->set_overlay_pixel_0(0, 0, 0, false);
    }
}

void LumeMode::loop_tick() {
    const uint32_t now = millis();

    // Epic 6C Phase F: pump every active binding's tick(). Bindings that
    // don't override stay no-op; tick-driven bindings (e.g. text scroll
    // animation post-Epic-13) drive their own continuous render here.
    // The Lume's loop_tick already runs at the main-loop cadence (~20 Hz
    // via the delay(1) yield + the audio cadence), which is fine for
    // smooth visual interpolation.
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (slot.binding && slot.ctx) {
            slot.binding->tick(*slot.ctx, now);
        }
    }

    // Drain any LIGHT_PULSE queued by the ESP-NOW callback. Doing
    // this here (main task context) keeps IRsend::sendRaw off the
    // WiFi task where it would crash the S3.
    if (pending_light_) {
        pending_light_ = false;
        fan_out_light_pulse(pending_light_payload_);
    }

    // Drain any pending repeater rebroadcast. Same deferred pattern -
    // ESP-NOW send from the WiFi callback context is unsafe in our
    // arduino-esp32 v2.x setup.
    // Epic 15: drain the signal-recovered repaint flag set by on_recv
    // when the Director comes back after a NO SIGNAL window. We do
    // this BEFORE the relay drain (and BEFORE draw_repeat_count below)
    // so the R counter gets painted on the cleared screen rather than
    // wiped by the recovery clear.
    if (signal_recovered_needs_repaint_) {
        signal_recovered_needs_repaint_ = false;
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        // Force draw_repeat_count to paint on its next call by
        // resetting the cache; otherwise the cleared screen leaves
        // the counter invisible because the cached value matches.
        last_drawn_repeats_ = 0xFFFFFFFFu;
        draw_status_pip();
        last_draw_ms_ = now;
    }

    if (pending_repeat_) {
        pending_repeat_ = false;
        if (auto* radio = hal::HAL::esp_now()) {
            radio->send_broadcast(pending_repeat_buf_, pending_repeat_len_);
            // Epic 15 bench follow-up: bump the visible repeater
            // counter + repaint immediately so the operator can
            // confirm the relay path fired without needing a
            // USB-attached serial monitor.
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

    // Edge into NO SIGNAL: paint the status UI immediately (the rest
    // of the screen below the strip is dead space anyway since no
    // pulses are arriving). Lume does NOT auto-promote and does NOT
    // run any visually distinctive idle effect - per show-coordination
    // discipline, a Lume that loses the Director should fail subtle
    // (NO SIGNAL text only) so a brief outage doesn't visually
    // fragment the show.
    //
    // last_rx_ms_ is written from the WiFi-task callback (on_recv),
    // read here from main task. If on_recv fires between this
    // function's `now = millis()` sample and the comparison below,
    // last_rx_ms_ can briefly be one or two ms ahead of `now`.
    // Without the saturating subtract that 1-2 ms turns into
    // ~UINT32_MAX, trips the > kNoSignalMs threshold, and we get
    // a spurious NO SIGNAL flicker every pulse cycle.
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

    // Fallback wash transitions (EMF prep). Two edges anchored to
    // age_since_rx so they fire exactly once per silence episode:
    //
    //   age > kFallbackEnterMs (10 s)  -> start blue/purple cycle wash
    //   age > kFallbackFadeStartMs (40 s) -> begin fade-to-black
    //
    // on_recv resets fallback_active_ + fallback_faded_ when a frame
    // arrives, so a returning Director cancels both. Guarded on
    // rx_count_ > 0 so cold-boot scanning doesn't trip the fallback
    // before any Director has ever been seen.
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

    // Three-channel scan in auto mode. Spec §5.3: rotate 11 (show)
    // -> 1 (hobby) -> 6 (advanced override) -> repeat, 2 s dwell each.
    // Scans on cold start (no frames received yet, Director could be
    // on any of the three channels) AND on Director-loss-past-rescan
    // threshold. The re-scan trigger uses kRescanMs (10 s), not
    // kNoSignalMs (3 s): NO SIGNAL displays quickly so the operator
    // sees the outage, but we wait longer before giving up on the
    // current channel because most outages are transient (Director
    // reboot, brief congestion, person blocking line of sight).
    // Any inbound frame in on_recv stops the scan implicitly because
    // it locks us to whichever channel we were on when the frame
    // arrived. Locked Lumes (lume_channel_pref_ != 0) never re-scan -
    // the operator picked that channel deliberately.
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
        // Channel change invalidates any TOFU lock - a Director on
        // the new channel has its own source_id, and we don't want
        // a stale lock from the prior channel rejecting the first
        // frame on the new one. clear() lets the next admit() on
        // the new channel re-establish.
        tofu_.clear();
        if (auto* radio = hal::HAL::esp_now()) {
            radio->set_channel(current_listen_chan_);
#ifdef ARDUINO
            Serial.printf("[espnow] lume scan -> ch=%u\n",
                          (unsigned)current_listen_chan_);
#endif
        }
    }

    // TOFU lock-expiry tick. Returns true exactly once on the
    // expiring edge (10 s of silence from the locked Director).
    // The next admitted frame on this channel re-establishes the
    // lock - so at a multi-show venue, if Director A goes silent
    // and Director B keeps broadcasting, the Lume drifts to B
    // within ~10 s. Logs the edge so the Director-loss diagnosis
    // surfaces in the serial monitor alongside NO SIGNAL and the
    // signal-loss-fallback wash transitions.
    if (tofu_.tick(now)) {
#ifdef ARDUINO
        Serial.println("[espnow] lume TOFU lock expired; ready to relock");
#endif
    }

    // Status pip refreshes at 10 Hz. The pip overlays a full-screen
    // pulse rect (Block 13 layout) so pulses can repaint over it
    // between refreshes; 10 Hz keeps the pip reading as steady even
    // when a vis is repainting underneath at ~30 Hz, while bounding
    // the SPI write rate (~7 fill_rects per pip refresh, batched
    // into one burst via begin_buffered_paint).
    if (now - last_pip_draw_ms_ > kPipRefreshMs) {
        draw_status_pip();
        last_pip_draw_ms_ = now;
    }

    // NO SIGNAL diagnostic body in the pulse-rect area (no pulses
    // arrive in that state so it stays visible). Refreshes the age
    // counter every ~200 ms.
    if (no_signal_ && now - last_draw_ms_ > 200) {
        draw_no_signal_body();
        last_draw_ms_ = now;
    }

    // LED-strip status overlay (Epic 12 B5, DRY refactor). On hosts
    // with a Display the LCD pip already carries this information;
    // on display-less hosts (Atom Lite) the LED strip is the only
    // surface, and pixel 0 doubles as the status indicator. Same
    // Lume state, different output sink - the variance is the HAL
    // capability query below, nothing about the signal logic itself.
    //
    //   Searching        flashing green at 1 Hz, 50 % duty
    //   FreshlyLocked    solid green for kFreshLockMs after a lock edge
    //   Active           overlay disabled; pixel 0 belongs to the wash
    if (hal::HAL::led_strip() != nullptr && hal::HAL::display() == nullptr) {
        auto* strip_drv = led_strip_driver_instance();
        if (flash_group_remaining_ > 0) {
            // Group-cycle confirmation: N white pulses on pixel 0.
            // Runs to completion regardless of signal state so the
            // operator always sees the count they just set. Each
            // toggle edge fires every kGroupFlashHalfPeriodMs; the
            // off half decrements the remaining counter, so a group
            // of N gives exactly N on/off pairs.
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

    // Btn1 LongPressed on display-less hosts (Atom Lite) cycles
    // lume_group_ through 1 -> 2 -> 3 -> 1. Sticks have Config >
    // Group as the canonical path and are excluded by the display
    // guard. Modulo (g % 3) + 1 also pulls values outside {1, 2, 3}
    // (e.g. 0 or a build-flag override outside the cycle range) back
    // into the cycle on the first press. Persists immediately; the
    // white pixel-0 flash sequence confirms the new value visually.
    // Compile-time gate lets deployments disable per-device runtime
    // regrouping; see lume_mode.h for the flag semantic.
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

    // Btn1 short-press brightness cycle was retired 2026-07-08 - the
    // strip is now hard-wired to 5 % at DAL::begin so no runtime
    // adjustment is meaningful, and on the Atom Lite the button is
    // needed for GroupID cycling (long-press above). Restored via a
    // per-host external-PSU cap raise + a build flag if a stage
    // deployment ever needs live brightness control.
}

bool LumeMode::seen_recently(uint8_t src, uint8_t seq) const {
    if (seq == 0) return false;
    for (size_t i = 0; i < kDedupRingSize; ++i) {
        if (dedup_ring_[i].source_id == src
         && dedup_ring_[i].sequence_number == seq) {
            return true;
        }
    }
    return false;
}

void LumeMode::mark_seen(uint8_t src, uint8_t seq) {
    if (seq == 0) return;
    dedup_ring_[dedup_head_] = DedupEntry{src, seq};
    dedup_head_ = (dedup_head_ + 1) % kDedupRingSize;
}

// -------------------------------------------------------------------------
// Lume-as-target-device: an inbound LIGHT_PULSE fans out to every
// active OutputBinding. Each binding owns one render surface (e.g.
// PixMobIrBinding -> IR to bracelets in the Lume's configured group;
// LumeLedStripBinding -> attached SK6812 strip via HAL::led_strip()).
// Bindings are fail-silent if their underlying transport / driver
// isn't enabled; nothing auto-forwards from inside render_fx - keeps
// each call to one job and respects toggles like IR mute (Config >
// IR > Enable, which gates the ir-pixmob driver via DAL::set_driver_enabled).
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

    // Epic 4.65 Block 5: class+group filter per binding.
    //   Class: target_class == 0 (All) OR matches the binding's class().
    //   Group: target_group == 0 (All) OR matches lume_group_ - but ONLY
    //          for local bindings. Relay bindings (PixMobIrBinding) bypass
    //          this check because their downstream protocol (PixMob IR)
    //          does its own group filtering at the bracelet level. The
    //          relay then reads the inbound target_group from the context
    //          and uses it as the downstream group code.
    //
    // Epic 13: bindings that declared can_render_in_callback() == true
    // were already dispatched inline from on_recv (in WiFi-task
    // context). Skip them here so they're not fired twice. Only
    // deferred-render bindings (PixMobIrBinding's blocking IRsend
    // path) reach this point.
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->can_render_in_callback()) continue;

        // Class filter.
        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        // Group filter (skipped for relay bindings).
        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // Thread the inbound addressing through to the binding via ctx.
        slot.ctx->set_current_target(p.target_class, p.target_group);
        slot.binding->on_light_pulse(*slot.ctx, ev);
    }
}

void LumeMode::fan_out_light_pulse_inline(
        const transport::espnow::LightPulsePayload& p) {
    // Mirror of fan_out_light_pulse but ONLY dispatching to bindings
    // that declared can_render_in_callback() == true. Called from
    // on_recv (WiFi task) right after decode, so the pulse start-time
    // each binding stamps lands within microseconds of the broadcast
    // landing on the radio - same instant on every Lume in the fleet.
    // Inter-Lume sync improves from "up to one loop_tick of phase
    // variance" (~50 ms at 20 Hz) to "radio-propagation + IRQ entry"
    // (~µs). The deferred path (fan_out_light_pulse, called from
    // loop_tick after pending_light_ flag drain) still handles
    // can_render_in_callback() == false bindings - notably PixMob's
    // IRsend, which would crash the WiFi task if invoked here.
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

        slot.ctx->set_current_target(p.target_class, p.target_group);
        slot.binding->on_light_pulse(*slot.ctx, ev);
    }
}

// Epic 6C Phase F: WASH-family fan-out. Same class+group filter as
// fan_out_light_pulse, with an additional capability gate: bindings that
// declare can_wash = false never see the WASH-family hooks. The binding
// owns its own wash state machine; this dispatch just routes the frame.
// LIGHT_WASH_PULSE is routed unconditionally to wash-capable bindings -
// the binding's handler drops silently if it has no active wash.

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

        slot.ctx->set_current_target(p.target_class, p.target_group);
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

        slot.ctx->set_current_target(p.target_class, p.target_group);
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

        slot.ctx->set_current_target(p.target_class, p.target_group);
        slot.binding->on_light_wash_pulse(*slot.ctx, ev);
    }
}

// Signal-loss fallback wash. Synthesises a LIGHT_WASH event locally
// and routes it through fan_out_light_wash so every wash-capable
// binding picks it up exactly as if the Director had emitted it.
// target_class = 0 (any), target_group = 0 (any) so the group filter
// in fan_out_light_wash admits the synthetic event regardless of the
// Lume's configured group. Local-only - never broadcast onto the
// radio (no encode + radio.send_broadcast path is taken).
void LumeMode::emit_fallback_wash_start() {
    transport::espnow::LightWashPayload p{};
    p.target_class   = 0;
    p.target_group   = 0;
    p.r1 = kFallbackColourA[0];  p.g1 = kFallbackColourA[1];  p.b1 = kFallbackColourA[2];
    p.r2 = kFallbackColourB[0];  p.g2 = kFallbackColourB[1];  p.b2 = kFallbackColourB[2];
    p.attack         = kFallbackAttackTicks;
    p.release        = 50;                              // 5 s default release if cancelled without an END
    p.intensity      = kFallbackIntensity;
    p.cycle_ms       = kFallbackCyclePeriodMs;
    p.ttl_seconds    = 0;                               // infinite; we'll END it explicitly
    p.pulse_response = 0;                               // suppress pulse overlay during the calm idle
    fan_out_light_wash(p);
#ifdef ARDUINO
    Serial.println("[espnow] lume FALLBACK wash start (blue/purple cycle)");
#endif
}

// Begin the 30 s fade-to-black phase. The LIGHT_WASH_END's
// release_time is a u8 in 100 ms units (max ~25.5 s) - we cap there;
// the user-requested 30 s is functionally the same length to the eye,
// and the alternative would be staging multiple END frames or a
// custom render path.
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

// Short, sharp fade-out when the Director comes back. Half a second
// removes the synthetic baseline before the returning Director's
// own wash/pulse traffic starts to compete with it.
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

// Epic 13: display-content fan-out. The Display family doesn't carry a
// target_class byte on the wire (the message type IS the class), so we
// filter bindings by device_class() == Display directly. Target group
// follows the Lume's local-binding rule: 0 = broadcast, non-zero must
// equal this Lume's configured group. No relay concept for Display
// bindings.
void LumeMode::fan_out_text_display(const transport::espnow::TextDisplayPayload& p) {
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;
        if (slot.binding->device_class() != hal::DeviceClass::Display) continue;
        if (p.target_group != 0 && p.target_group != lume_group_) continue;
        slot.ctx->set_current_target(
            static_cast<uint8_t>(hal::DeviceClass::Display), p.target_group);
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
            static_cast<uint8_t>(hal::DeviceClass::Display), p.target_group);
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
            static_cast<uint8_t>(hal::DeviceClass::Display), p.target_group);
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
            static_cast<uint8_t>(hal::DeviceClass::Display), p.target_group);
        slot.binding->on_clear_screen(*slot.ctx, p);
    }
}

void LumeMode::on_recv(const hal::ESPNowMessage& m) {
    using namespace transport::espnow;

    // Parse the header first. A malformed frame (magic mismatch /
    // version mismatch / etc.) is dropped before any liveness or
    // TOFU update - it's not a NocturNation frame, so it doesn't
    // count as our Director being alive.
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

    // Spec §4.3: frames whose hop_count exceeds the mesh limit
    // (kMaxHopCount = 3) MUST be dropped before admission. Matches the
    // Tildagon Lume (nocturnation/receive.py: MAX_HOP_COUNT gate) and
    // prevents a hop-mangled attacker frame from establishing a false
    // TOFU lock. Frames at exactly kMaxHopCount still render but the
    // relay gate at line ~900 refuses to re-broadcast them.
    if (hdr.hop_count > kMaxHopCount) {
#ifdef ARDUINO
        Serial.printf("[espnow RX HOP DROP] hop=%u src=%02X seq=%u\n",
                      hdr.hop_count, hdr.source_id, hdr.sequence_number);
#endif
        return;
    }

    // TOFU lock (EMF prep 2026-06-24): partition the Lume between
    // potentially-multiple Directors broadcasting on the same channel.
    // First-eligible-frame establishes the lock; subsequent frames
    // from any other Director are silently dropped. Display-family
    // broadcasts (orchestrator-origin, source_id = 0xFF) are admitted
    // once a Director session exists, without resetting the
    // liveness timer. This is the StickC port of the Tildagon's
    // TofuLock - without it a StickC-based Lume at EMF would
    // render BOTH shows' content simultaneously.
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

    // Past the TOFU gate: this frame is from our Director (or an
    // orchestrator-bridged display broadcast under a held lock).
    // Count it toward Director-liveness. Includes duplicates -
    // redundant retransmits should refresh the liveness timer the
    // same way unique frames do.
    rx_count_++;
    last_rx_ms_ = admit_now_ms;

    const bool was_no_signal = no_signal_;
    no_signal_ = false;

    // Cancel the fallback wash if one was running. emit_fallback_wash_recovery
    // sends a short-release LIGHT_WASH_END so the binding fades the
    // synthetic baseline out cleanly before the Director's returning
    // wash/pulse traffic starts populating the surface. Flags reset
    // here so the next silence episode re-triggers from scratch.
    if (fallback_active_) {
        fallback_active_ = false;
        fallback_faded_  = false;
        emit_fallback_wash_recovery();
    }

    // Stamp lock_acquired_ms_ on the first frame ever and on the
    // recovery edge out of a NO SIGNAL window. Drives the brief
    // "freshly locked" indication on the LED-strip status overlay
    // (and matches the natural moment to flash a future LCD pip).
    if (was_no_signal || rx_count_ == 1) {
        lock_acquired_ms_ = last_rx_ms_;
    }
    if (was_no_signal) {
#ifdef ARDUINO
        Serial.println("[espnow] lume SIGNAL RECOVERED");
#endif
        // Epic 15 bench-test bug fix. Pre-fix, the screen stayed stuck
        // on NO SIGNAL even after frames resumed - which looked like a
        // contradiction during the corner test ("R counter incrementing
        // but NO SIGNAL displayed"). The receiver was fine; the LCD was
        // just stale. Defer the repaint to loop_tick via this flag
        // because SPI paints from the WiFi task are crash-prone on the
        // S3 (same reasoning as pending_repeat_).
        signal_recovered_needs_repaint_ = true;
    }

    last_source_id_ = hdr.source_id;
    last_msg_type_  = static_cast<uint8_t>(hdr.message_type);

    // Deduplication gate: if we've already processed this exact
    // (source_id, sequence_number) within the last 16 frames, log
    // and drop. Prevents the Director's 2-3x redundant TX (per spec
    // §4.3 reliability strategy, lands in Block 5) from causing
    // double IR fires / double screen paints per logical beat.
    const bool is_dup = seen_recently(hdr.source_id, hdr.sequence_number);
    if (!is_dup) {
        mark_seen(hdr.source_id, hdr.sequence_number);
        // Quality tracker only counts unique frames - duplicates from
        // the Director's redundancy-for-reliability TX shouldn't make
        // the signal look better than it actually is.
        quality_.note_frame(hdr.source_id,
                            hdr.sequence_number,
                            last_rx_ms_);
    }

#ifdef ARDUINO
    // Per-frame RX log. Pared down to "command only" - source_id and
    // sequence_number are recoverable from the orchestrator-side emit
    // log if needed for correlation, but at peak traffic (wash +
    // sparkles + 3x display retransmit, 150+ Hz) every byte of Serial
    // output adds latency to the WiFi callback. The DUP marker is
    // retained: it's the single most useful piece of diagnostic
    // (radio retransmit correlation) and free. ~8 chars/frame is
    // a ~4x reduction over the prior ~30-char line.
    Serial.printf(is_dup ? "[RXD %02X]\n" : "[RX %02X]\n",
                  (unsigned)last_msg_type_);
#endif

    if (is_dup) return;

    // Repeater mode (spec §4.3): rebroadcast each unique frame with
    // hop_count incremented by 1, up to a 3-hop ceiling. Preserves
    // source_id + sequence_number so dedup works mesh-wide. Defer
    // the actual radio.send_broadcast to loop_tick (same WiFi-task
    // safety reasoning as the IR forward path).
    if (lume_repeat_en_
        && hdr.hop_count < kMaxHopCount
        && m.len <= kRepeatBufSize) {
        std::memcpy(pending_repeat_buf_, m.data, m.len);
        // Bump hop_count in place at its v2 wire offset. Named constant +
        // helper live in transport/espnow/frame.h so the offset can't
        // drift from the header layout again (v1 was byte 3, v2 is byte 5
        // - the 2-byte magic + version prefix shifted every field). Fix
        // history: pre-2026-06-28 this wrote to buf_[3], corrupting
        // source_id and breaking mesh-wide dedup + TOFU lock.
        transport::espnow::set_hop_count(pending_repeat_buf_, m.len,
                                          hdr.hop_count + 1);
        pending_repeat_len_    = m.len;
        pending_repeat_        = true;
    }

    // Display-as-light: defer LIGHT_PULSE rendering to loop_tick.
    // This callback runs on the ESP-NOW / WiFi task; render_light
    // fans out to render_fx("all-pixmobs"), which calls into
    // IRsend::sendRaw - a ~30 ms blocking GPIO toggle loop unsafe
    // to run from the WiFi task (causes watchdog / stack issues
    // on the S3). Copying the payload is fast and safe; loop_tick
    // pumps it from main task context. Newer arrivals replace
    // older ones - dropping a stale beat is fine when a fresh one
    // is already on the way.
    if (hdr.message_type == MessageType::LightPulse
        && m.len == kHeaderSize + kLightPulsePayloadLen) {
        LightPulsePayload p{};
        if (decode_light_pulse(hdr, m.data + kHeaderSize,
                                 m.len - kHeaderSize, p)
            == DecodeResult::Ok) {
            // Epic 13 sync fix: stamp callback-safe renderers (LED
            // strip, LCD) IMMEDIATELY in WiFi-task context. All
            // Lumes anchor their pulse envelope to broadcast-receipt
            // time, which is ~µs synchronised across the fleet.
            // Inter-Lume render unison for sparkle-on-beat goes from
            // up-to-50 ms loop-tick variance to ~µs radio-propagation
            // variance.
            fan_out_light_pulse_inline(p);

            // Blocking renderers (PixMobIrBinding → IRsend::sendRaw,
            // ~30 ms cli/sei GPIO bit-bang) stay on the deferred
            // path: queue here, drain in loop_tick on the main task.
            // fan_out_light_pulse() filters out can_render_in_callback
            // bindings so they aren't double-fired.
            pending_light_payload_ = p;
            pending_light_ = true;
        }
    }

    // Epic 6C Phase F: WASH-family dispatch. Phase D's log-and-discard
    // stubs are replaced here with capability-gated fan-out to active
    // bindings (LumeLedStripBinding + PixMobIrBinding accept the wash
    // family; bindings that declare can_wash = false are silently
    // filtered out by the fan-out).
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

    // Epic 13 B1: display-content family dispatch. Decode only -
    // the rendering bindings (LumeTextBinding, LumeBitmapBinding)
    // land in B2 / B6. Quick-drop gate first: any message type that
    // maps to a specific capability the host lacks is silently
    // discarded here without decoding the payload. Atom Lite (no
    // Display) hits this path for every inbound TEXT/BITMAP/CLEAR
    // and pays only the message_type_required_capability lookup.
    const hal::Capability req_cap =
        message_type_required_capability(hdr.message_type);
    if (req_cap != kNoSpecificCapability && !hal::HAL::has(req_cap)) {
        return;
    }

    // Display family dispatch. Trimmed log: just the type-confirmation
    // is enough at this layer - the brief "[espnow RX 09 src=N seq=N]"
    // above already records receipt. Full hdr/body content can be up
    // to 192 bytes -> ~22 ms of blocking Serial.printf at 115200 baud
    // per frame -> with 3x retransmit per emission that's ~66 ms of
    // WiFi-task block per lyric, which lets the IDF receive queue
    // overflow and drops subsequent broadcasts. Operators wanting to
    // verify content correlate this brief log with the orchestrator's
    // `display: TEXT_DISPLAY sent header='...' body='...'` line.
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

// ---------------------------------------------------------------------
// Status pip: always-visible 38x12 px overlay anchored to the top-right
// corner. Compact horizontal arrangement - small coloured signal dot
// (red/amber/green per bar count) plus a battery glyph. Block 13
// replaced the previous full-width 12 px strip with this pip so the
// pulse rect can run full-screen for maximum show impact. The pip
// paints over whatever the pulse last drew underneath it; pulses may
// repaint over the pip between pip refreshes - the 10 Hz pip cadence
// in loop_tick is fast enough that operators read the pip as steady.
// ---------------------------------------------------------------------

// Frame-age proxy. Used as a cold-start fallback before the quality
// tracker has accumulated enough data for a real estimate, and as the
// post-NO-SIGNAL killer (any bar count is meaningless if the Director
// is gone entirely).
int LumeMode::signal_bars_from_age() const {
    if (rx_count_ == 0)              return 0;
    // Saturating subtract: handle the WiFi-task / main-task race where
    // last_rx_ms_ can briefly be slightly ahead of millis() because
    // on_recv fired between the caller's millis() sample and this read.
    const uint32_t now = millis();
    const uint32_t age = (now >= last_rx_ms_) ? (now - last_rx_ms_) : 0;
    if (age < 500)                   return 4;
    if (age < 1000)                  return 3;
    if (age < 2000)                  return 2;
    if (age < kNoSignalMs)           return 1;
    return 0;
}

// Combined signal-bar count. Primary metric is sequence-loss-rate
// (transport::SignalQuality), which reflects delivered fidelity -
// what an operator actually cares about for show coordination.
// Falls back to the age proxy in two cases:
//   - Cold start: not enough frames received yet for a meaningful
//     loss percentage.
//   - NO SIGNAL: frame age beats whatever the loss tracker says,
//     because no recent frames means no current signal regardless
//     of historical fidelity.
int LumeMode::signal_bars() const {
    if (no_signal_ || rx_count_ == 0)            return 0;
    const int q = quality_.bars(millis());
    if (q < 0)                                    return signal_bars_from_age();
    const int a = signal_bars_from_age();
    return (q < a) ? q : a;
}

void LumeMode::draw_status_pip() {
    // Buffered paint session: the ~7 fill_rects that compose the pip
    // batch into a single sprite, then push to the panel as one SPI
    // burst. Without this each op writes independently to the panel
    // and a panel scan-out crossing one of those windows would show
    // tear lines between elements.
    auto* ld = dal::local_driver_instance();
    const bool buffered =
        ld->begin_buffered_paint(kPipX, kPipY, kPipWidth, kPipHeight);

    // Wipe the pip black. The pip needs an opaque background because
    // a full-screen pulse may have painted any colour underneath it
    // since our last refresh; without the wipe, signal/battery
    // colours would composite against arbitrary backgrounds.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        kPipX, kPipY, kPipWidth, kPipHeight, BLACK});

    // Signal dot on the left of the pip. ~6 px diameter using a 6x6
    // fill_rect (square approximation is fine at this size). Colour
    // codes per spec: 4 bar = green, 2-3 bar = amber, 0-1 bar = red.
    const int bars       = signal_bars();
    const uint16_t dot_c = (bars >= 4) ? GREEN
                         : (bars >= 2) ? YELLOW
                         :               RED;
    const int dot_x = kPipX + 2;
    const int dot_y = kPipY + 3;
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        dot_x, dot_y, 6, 6, dot_c});

    // Battery glyph on the right: 16 px wide outline + tip + fill.
    // Sits flush against the right edge of the pip with the tip
    // poking just past kPipWidth - 2, well within the 38 px budget.
    const int batt_w = 14;             // body width
    const int batt_h = 8;
    const int batt_x = kPipX + kPipWidth - batt_w - 2;   // 2 px tip + edge
    const int batt_y = kPipY + 2;
    // Outline (1-px fill_rects on all four sides).
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y, batt_w, 1, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y + batt_h - 1, batt_w, 1, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x, batt_y, 1, batt_h, WHITE});
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x + batt_w - 1, batt_y, 1, batt_h, WHITE});
    // Tip on the right.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        batt_x + batt_w, batt_y + 2, 2, batt_h - 4, WHITE});

    // Fill proportion.
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

    // Epic 15 bench follow-up: paint the relay counter beneath the
    // pip (or skip if repeat mode is off). Paint sits outside the pip
    // buffer because it's a separate screen region; lazy repaint on
    // count change keeps SPI traffic low even when frames stream in.
    draw_repeat_count();
}

void LumeMode::draw_repeat_count() {
    // No-op when the operator hasn't enabled repeater mode - keeps
    // a non-repeater Lume's LCD clean.
    if (!lume_repeat_en_) {
        // First-time entry when the operator just toggled OFF: paint
        // a wipe so any leftover "R:NNN" from a previous session is
        // erased. We only do this once via last_drawn_repeats_ ==
        // sentinel; subsequent draws are no-op.
        if (last_drawn_repeats_ != 0xFFFFFFFEu) {
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                0, 120, 80, 12, BLACK});
            last_drawn_repeats_ = 0xFFFFFFFEu;
        }
        return;
    }
    if (repeats_emitted_ == last_drawn_repeats_) return;
    last_drawn_repeats_ = repeats_emitted_;

    // Wipe just the digit area (avoids paint compositing against a
    // previous longer count) then write the new value.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        0, 120, 80, 12, BLACK});

    char line[16];
    std::snprintf(line, sizeof(line), "R:%lu",
                  (unsigned long)repeats_emitted_);
    // Size 1 text at the bottom-left. ~8 px tall, ~6 px wide per char,
    // so "R:99999" fits in 42 px - well inside the 80 px wipe.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        2, 122, line, GREEN, BLACK, 1});
}

// Diagnostic body shown only while NO SIGNAL is active. No incoming
// pulses are arriving in this state so the whole screen below the pip
// is ours to spend on text. Block 13 reflowed this for hierarchy:
// size-3 "NO SIGNAL" headline near the top (just below the pip),
// then a related group of size-1 diagnostic lines (channel, rx total,
// last rx age), then the gesture hint anchored at the bottom.
void LumeMode::draw_no_signal_body() {
    char line[40];

    // Headline: centred horizontally just below the pip. Size-3 char
    // cell is 18 px wide; "NO SIGNAL" is 9 chars -> 162 px wide ->
    // (240 - 162) / 2 = 39 px left margin.
    constexpr int kHeadlineX = (240 - 9 * 18) / 2;
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kHeadlineX, 20, "NO SIGNAL", RED, BLACK, 3});

    // Diagnostic block. 14 px line spacing keeps related lines grouped
    // without crowding (size-1 char height is 8 px; 14 px gives breathing
    // room and aligns visually with the headline-to-body gap).
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

    // Gesture hint anchored at the bottom of the screen. LumeMode is
    // still on raw ButtonEvent (it hasn't migrated to InputAction yet),
    // and its on_button_event routes Btn2 LongPressed straight to
    // ModeMachine::switch_to(Menu) - so "B-hold: menu" is literally
    // what the gesture does.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kDiagX, 122, "B-hold: menu", WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
