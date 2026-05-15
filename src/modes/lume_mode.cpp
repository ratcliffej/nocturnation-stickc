// LumeMode implementation.

#include "lume_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_rect / pulse_enabled
#include "output_bindings/output_binding_registry.h"
#include "output_bindings/local_display.h"
#include "output_bindings/pixmob_ir.h"

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
using nocturnation::output_bindings::local_display_instance;
using nocturnation::output_bindings::local_display_context;

namespace {

// Per-binding context lookup. The framework owns one OutputBindingContext
// per registered binding singleton (declared alongside the binding's
// instance + property_bag accessors). The walk in enter() pairs each
// binding with its own context here. Falls back to nullptr for any
// binding that wasn't expected at registration time - those still get
// activated but with no per-binding context (their on_light_command
// has to operate context-free or skip work; the two we ship today
// always have a context).
OutputBindingContext* context_for(const OutputBinding* binding) {
    if (binding == pixmob_ir_instance())      return &pixmob_ir_context();
    if (binding == local_display_instance())  return &local_display_context();
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
    last_pip_draw_ms_ = 0;

    // Load operator-configured preferences from NVS. Channel preference
    // picks hobby (1) / show (11) / advanced (6) / auto-scan (0) per
    // spec §4.5. The slv_ir_grp setting moved to PixMobIrBinding's
    // property bag in Block 9; ConfigMode > IR > Lume Group mutates
    // it there and on_light_command reads it inline.
    lume_channel_pref_  = persistence::load_lume_channel();
    lume_repeat_en_     = persistence::load_lume_repeat_enabled();
    lume_group_           = persistence::load_lume_group();
    quality_.reset();

    // Auto-scan starts on channel 11 (show priority) per spec §4.5.
    // Locked configs start on the configured channel.
    current_listen_chan_  = (lume_channel_pref_ == 0)
                            ? 11
                            : lume_channel_pref_;
    last_chan_switch_ms_  = millis();

    // Block 13: pulse rect runs full-screen. The 38x12 px status pip
    // (top-right corner) paints OVER the pulse on its own cadence; the
    // pulse may briefly paint underneath between pip refreshes, which
    // is accepted in exchange for full-screen colour impact during a
    // show. reset_pulse_rect() is the explicit "no strip exclusion"
    // signal - LocalDriver's default is also full-screen, but calling
    // it makes the contract obvious to readers and survives any
    // future change to the default.
    dal::local_driver_instance()->reset_pulse_rect();

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    draw_status_pip();      // initial paint so the pip exists

    // Activate output bindings whose required capabilities the host
    // supports. Walks the registry, gates each one on
    // required_capabilities().subset_of(host_caps), and calls
    // binding->enter(ctx) on each accepted entry. Block 9 ships two
    // bindings (LocalDisplayBinding + PixMobIrBinding); the soft cap
    // of kMaxActiveBindings (4) gives near-future bindings room.
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
}

void LumeMode::loop_tick() {
    const uint32_t now = millis();

    // Drain any LIGHT_COMMAND queued by the ESP-NOW callback. Doing
    // this here (main task context) keeps IRsend::sendRaw off the
    // WiFi task where it would crash the S3.
    if (pending_light_) {
        pending_light_ = false;
        fan_out_light_command(pending_light_payload_);
    }

    // Drain any pending repeater rebroadcast. Same deferred pattern -
    // ESP-NOW send from the WiFi callback context is unsafe in our
    // arduino-esp32 v2.x setup.
    if (pending_repeat_) {
        pending_repeat_ = false;
        if (auto* radio = hal::HAL::esp_now()) {
            radio->send_broadcast(pending_repeat_buf_, pending_repeat_len_);
#ifdef ARDUINO
            Serial.printf("[espnow REPEAT hop=%u] ",
                          (unsigned)pending_repeat_buf_[3]);
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

    // Dual-channel scan in auto mode. Spec §4.5: alternate channel
    // 11 (show priority) and channel 1 (hobby), 2 s dwell each.
    // Scans on cold start (no frames received yet, Director could be
    // on either channel) AND on Director-loss (no_signal_). Any
    // inbound frame in on_recv stops the scan implicitly because
    // it locks us to whichever channel we were on when the frame
    // arrived.
    const bool scanning = (lume_channel_pref_ == 0)
                       && (rx_count_ == 0 || no_signal_);
    if (scanning && (now - last_chan_switch_ms_) >= kChannelDwellMs) {
        current_listen_chan_ = (current_listen_chan_ == 11) ? 1 : 11;
        last_chan_switch_ms_ = now;
        if (auto* radio = hal::HAL::esp_now()) {
            radio->set_channel(current_listen_chan_);
#ifdef ARDUINO
            Serial.printf("[espnow] lume scan -> ch=%u\n",
                          (unsigned)current_listen_chan_);
#endif
        }
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
}

void LumeMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        ModeMachine::switch_to(ModeId::Menu);
    }
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
// Lume-as-target-device: an inbound LIGHT_COMMAND fans out to every
// active OutputBinding. Each binding owns one render surface (e.g.
// LocalDisplayBinding -> screen on the StickC; PixMobIrBinding -> IR
// to bracelets in the Lume's configured group). Bindings are
// fail-silent if their underlying transport / driver isn't enabled;
// neither auto-forwards from inside render_fx - keeps each call to one
// job and respects toggles like IR mute (Config > IR > Enable, which
// gates the ir-pixmob driver via DAL::set_driver_enabled).
// -------------------------------------------------------------------------

void LumeMode::fan_out_light_command(const transport::espnow::LightCommandPayload& p) {
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
    for (size_t i = 0; i < active_binding_count_; ++i) {
        const auto& slot = active_bindings_[i];
        if (!slot.binding || !slot.ctx) continue;

        // Class filter.
        const uint8_t binding_class = static_cast<uint8_t>(slot.binding->device_class());
        if (p.target_class != 0 && p.target_class != binding_class) continue;

        // Group filter (skipped for relay bindings).
        if (!slot.binding->is_relay()) {
            if (p.target_group != 0 && p.target_group != lume_group_) continue;
        }

        // Thread the inbound addressing through to the binding via ctx.
        slot.ctx->set_current_target(p.target_class, p.target_group);
        slot.binding->on_light_command(*slot.ctx, ev);
    }
}

void LumeMode::on_recv(const hal::ESPNowMessage& m) {
    using namespace transport::espnow;

    // Any frame received - including duplicates - counts as the
    // Director being alive. Update rx_count_ and last_rx_ms_ before
    // the dedup gate so Director-loss detection isn't fooled by
    // redundant retransmissions.
    rx_count_++;
    last_rx_ms_ = millis();

    const bool was_no_signal = no_signal_;
    no_signal_ = false;
    if (was_no_signal) {
#ifdef ARDUINO
        Serial.println("[espnow] lume SIGNAL RECOVERED");
#endif
    }

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
    Serial.printf("[espnow RX %s%02X src=%u seq=%u] ",
                  is_dup ? "DUP " : "",
                  (unsigned)last_msg_type_,
                  (unsigned)hdr.source_id,
                  (unsigned)hdr.sequence_number);
    for (size_t i = 0; i < m.len && i < 32; ++i) {
        Serial.printf("%02X ", m.data[i]);
    }
    Serial.println();
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
        // hop_count is the 4th byte of the header per spec §4.3.
        pending_repeat_buf_[3] = hdr.hop_count + 1;
        pending_repeat_len_    = m.len;
        pending_repeat_        = true;
    }

    // Display-as-light: defer LIGHT_COMMAND rendering to loop_tick.
    // This callback runs on the ESP-NOW / WiFi task; render_light
    // fans out to render_fx("all-pixmobs"), which calls into
    // IRsend::sendRaw - a ~30 ms blocking GPIO toggle loop unsafe
    // to run from the WiFi task (causes watchdog / stack issues
    // on the S3). Copying the payload is fast and safe; loop_tick
    // pumps it from main task context. Newer arrivals replace
    // older ones - dropping a stale beat is fine when a fresh one
    // is already on the way.
    if (hdr.message_type == MessageType::LightCommand
        && m.len == kHeaderSize + kLightCommandPayloadLen) {
        LightCommandPayload p{};
        if (decode_light_command(hdr, m.data + kHeaderSize,
                                 m.len - kHeaderSize, p)
            == DecodeResult::Ok) {
            pending_light_payload_ = p;
            pending_light_ = true;
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
