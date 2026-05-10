// SlaveMode implementation.

#include "slave_mode.h"

#include "dal/dal.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_rect / pulse_enabled

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

namespace {

// =============================================================================
// Slave-only NVS helpers (slv_ir_grp, slv_chan, slv_repeat).
//
// Live here rather than in the shared persistence module because no other
// mode reads or writes them - keeping them local matches "the mode that
// owns the setting also owns the NVS key" discipline.
// =============================================================================

#ifdef ARDUINO
// Slave-mode IR forward group. 0 = broadcast (all-pixmobs), 1..5 = specific
// PixMob group. Defaults to 0 to preserve the historical broadcast behaviour;
// operators with multiple slaves in one venue should configure each one to a
// different group via Config > IR > Slave Group to avoid IR airspace fights.
uint8_t load_slave_ir_group() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t g = prefs.getUChar("slv_ir_grp", 0);
    prefs.end();
    if (g > 5) g = 0;
    return g;
}

uint8_t load_slave_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("slv_chan", 0);   // 0 = auto/scan
    prefs.end();
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    return c;
}

// Slave repeater mode: when enabled, slave rebroadcasts each unique
// frame with hop_count + 1 (capped at spec §4.3's 3-hop limit). Off
// by default - operator opts in for venue range extension. Persisted
// as `slv_repeat`.
bool load_slave_repeat_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("slv_repeat", false);   // default OFF
    prefs.end();
    return e;
}
#else
uint8_t load_slave_ir_group()       { return 0; }
uint8_t load_slave_channel()        { return 0; }
bool    load_slave_repeat_enabled() { return false; }
#endif

}  // namespace

void SlaveMode::enter() {
    rx_count_         = 0;
    last_rx_ms_       = 0;
    last_source_id_   = 0;
    last_msg_type_    = 0xFF;
    radio_active_     = false;
    no_signal_        = false;
    last_strip_draw_ms_ = 0;

    // Load operator-configured preferences from NVS. IR group lets
    // multiple slaves in one venue avoid IR airspace fights; channel
    // preference picks hobby (1) / show (11) / advanced (6) / auto-
    // scan (0) per spec §4.5.
    slave_ir_group_      = load_slave_ir_group();
    slave_channel_pref_  = load_slave_channel();
    slave_repeat_en_     = load_slave_repeat_enabled();
    quality_.reset();

    // Auto-scan starts on channel 11 (show priority) per spec §4.5.
    // Locked configs start on the configured channel.
    current_listen_chan_  = (slave_channel_pref_ == 0)
                            ? 11
                            : slave_channel_pref_;
    last_chan_switch_ms_  = millis();

    // Reserve a 12 px status strip at the top of the screen so the
    // battery + signal-strength icons stay visible while pulses paint
    // the rest of the screen. LocalDriver paints fill_rect within
    // these bounds; the strip is ours to draw into.
    dal::local_driver_instance()->set_pulse_rect(
        0, kStripHeight, 240, 135 - kStripHeight);

    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    draw_status_strip();    // initial paint so the strip exists

    if (auto* radio = hal::HAL::esp_now()) {
        radio->set_recv_callback([this](const hal::ESPNowMessage& m) {
            this->on_recv(m);
        });
        radio_active_ = radio->begin(current_listen_chan_);
#ifdef ARDUINO
        if (!radio_active_) {
            Serial.println("[espnow] slave begin() failed");
        } else {
            Serial.printf("[espnow] slave up: ch=%u (pref=%s, ir_grp=%u)\n",
                          (unsigned)current_listen_chan_,
                          slave_channel_pref_ == 0 ? "auto"
                          : slave_channel_pref_ == 1 ? "1 hobby"
                          : slave_channel_pref_ == 11 ? "11 show"
                          : "6 custom",
                          (unsigned)slave_ir_group_);
        }
#endif
    }
}

void SlaveMode::exit() {
    if (radio_active_) {
        if (auto* radio = hal::HAL::esp_now()) radio->end();
        radio_active_ = false;
    }
    // Restore full-screen pulse rect for whichever mode comes next.
    dal::local_driver_instance()->reset_pulse_rect();
}

void SlaveMode::loop_tick() {
    const uint32_t now = millis();

    // Drain any LIGHT_COMMAND queued by the ESP-NOW callback. Doing
    // this here (main task context) keeps IRsend::sendRaw off the
    // WiFi task where it would crash the S3.
    if (pending_light_) {
        pending_light_ = false;
        render_light(pending_light_payload_);
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
    // pulses are arriving). Slave does NOT auto-promote and does NOT
    // run any visually distinctive idle effect - per show-coordination
    // discipline, a slave that loses the master should fail subtle
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
        Serial.printf("[espnow] slave NO SIGNAL: %lu ms since last RX\n",
                      (unsigned long)age_since_rx);
#endif
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        draw_status_strip();
        draw_no_signal_body();
        last_draw_ms_ = now;
        return;
    }

    // Dual-channel scan in auto mode. Spec §4.5: alternate channel
    // 11 (show priority) and channel 1 (hobby), 2 s dwell each.
    // Scans on cold start (no frames received yet, master could be
    // on either channel) AND on master-loss (no_signal_). Any
    // inbound frame in on_recv stops the scan implicitly because
    // it locks us to whichever channel we were on when the frame
    // arrived.
    const bool scanning = (slave_channel_pref_ == 0)
                       && (rx_count_ == 0 || no_signal_);
    if (scanning && (now - last_chan_switch_ms_) >= kChannelDwellMs) {
        current_listen_chan_ = (current_listen_chan_ == 11) ? 1 : 11;
        last_chan_switch_ms_ = now;
        if (auto* radio = hal::HAL::esp_now()) {
            radio->set_channel(current_listen_chan_);
#ifdef ARDUINO
            Serial.printf("[espnow] slave scan -> ch=%u\n",
                          (unsigned)current_listen_chan_);
#endif
        }
    }

    // Status strip refreshes ~2x per second; the icons read battery
    // level + signal age both of which change slowly enough that
    // higher refresh rates would just waste SPI cycles.
    if (now - last_strip_draw_ms_ > kStripRefreshMs) {
        draw_status_strip();
        last_strip_draw_ms_ = now;
    }

    // NO SIGNAL diagnostic body in the pulse-rect area (no pulses
    // arrive in that state so it stays visible). Refreshes the age
    // counter every ~200 ms.
    if (no_signal_ && now - last_draw_ms_ > 200) {
        draw_no_signal_body();
        last_draw_ms_ = now;
    }
}

void SlaveMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        ModeMachine::switch_to(ModeId::Menu);
    }
}

bool SlaveMode::seen_recently(uint8_t src, uint8_t seq) const {
    if (seq == 0) return false;
    for (size_t i = 0; i < kDedupRingSize; ++i) {
        if (dedup_ring_[i].source_id == src
         && dedup_ring_[i].sequence_number == seq) {
            return true;
        }
    }
    return false;
}

void SlaveMode::mark_seen(uint8_t src, uint8_t seq) {
    if (seq == 0) return;
    dedup_ring_[dedup_head_] = DedupEntry{src, seq};
    dedup_head_ = (dedup_head_ + 1) % kDedupRingSize;
}

// -------------------------------------------------------------------------
// Slave-as-target-device: an inbound LIGHT_COMMAND fans out to every
// locally-available lighting output via render_fx, each fail-silent if
// its transport / driver isn't enabled. No auto-forwarding inside
// render_fx itself - keeps each call to one job and respects the IR
// mute toggle (Config > IR > Enable, which gates the ir-pixmob driver
// via DAL::set_driver_enabled).
//
// Current targets:
//   "local"       -> screen on the StickC; future LED on simpler devices,
//                    screen + onboard LEDs on Tildagon.
//   "all-pixmobs" -> IR broadcast to PixMob bracelets in range. TEMPORARY
//                    target choice for Block 3.5 - Block 4 should switch
//                    to "group-N" where N is the slave's NVS-configured
//                    group, so two slaves on different groups don't fight
//                    over the same airspace. Brand-independent rename of
//                    "all-pixmobs" / "group-N" defers to its own focused
//                    refactor pass; see project_pixmob_free_endgame memory.
// -------------------------------------------------------------------------

void SlaveMode::render_light(const transport::espnow::LightCommandPayload& p) {
    RgbPulseEvent ev{};
    ev.r       = p.r;
    ev.g       = p.g;
    ev.b       = p.b;
    ev.attack  = static_cast<pixmob::Time>(p.attack);
    ev.sustain = static_cast<pixmob::Time>(p.sustain);
    ev.release = static_cast<pixmob::Time>(p.release);
    ev.chance  = static_cast<pixmob::Chance>(p.chance);

    // Local light surface (screen on the StickC).
    DAL::render_fx("local", ev);

    // IR forward to bracelets in this slave's configured group.
    // Group 0 = broadcast to all PixMobs (compatible with bracelets
    // that haven't been programmed with a specific group); 1..5 =
    // specific group, lets two slaves in the same venue avoid
    // bombarding all bracelets in IR range. Operator picks via
    // Config > IR > Slave Group; persisted to NVS as slv_ir_grp.
    // Fail-silent if IR is muted (Config > IR > Enable) or this
    // host has no IR Tx capability.
    const char* ir_target = ir_target_name(slave_ir_group_);
    DAL::render_fx(ir_target, ev);
}

// Map group id (0..5) -> registered DAL device name. Group 0 maps to
// "all-pixmobs" for full-broadcast behaviour; 1..5 map to the per-group
// devices DAL::begin() registers.
const char* SlaveMode::ir_target_name(uint8_t group_id) {
    switch (group_id) {
        case 0:  return "all-pixmobs";
        case 1:  return "group-1";
        case 2:  return "group-2";
        case 3:  return "group-3";
        case 4:  return "group-4";
        case 5:  return "group-5";
        default: return "all-pixmobs";
    }
}

void SlaveMode::on_recv(const hal::ESPNowMessage& m) {
    using namespace transport::espnow;

    // Any frame received - including duplicates - counts as the
    // master being alive. Update rx_count_ and last_rx_ms_ before
    // the dedup gate so master-loss detection isn't fooled by
    // redundant retransmissions.
    rx_count_++;
    last_rx_ms_ = millis();

    const bool was_no_signal = no_signal_;
    no_signal_ = false;
    if (was_no_signal) {
#ifdef ARDUINO
        Serial.println("[espnow] slave SIGNAL RECOVERED");
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
    // and drop. Prevents the master's 2-3x redundant TX (per spec
    // §4.3 reliability strategy, lands in Block 5) from causing
    // double IR fires / double screen paints per logical beat.
    const bool is_dup = seen_recently(hdr.source_id, hdr.sequence_number);
    if (!is_dup) {
        mark_seen(hdr.source_id, hdr.sequence_number);
        // Quality tracker only counts unique frames - duplicates from
        // the master's redundancy-for-reliability TX shouldn't make
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
    if (slave_repeat_en_
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
// Status strip: always-visible 12 px band at the top of the screen.
// Battery icon on the right, 4-bar signal indicator just left of it.
// Painted into pixels (0,0)..(239,11) which are excluded from the
// LocalDriver's pulse rect.
// ---------------------------------------------------------------------

// Frame-age proxy. Used as a cold-start fallback before the quality
// tracker has accumulated enough data for a real estimate, and as the
// post-NO-SIGNAL killer (any bar count is meaningless if the master
// is gone entirely).
int SlaveMode::signal_bars_from_age() const {
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
int SlaveMode::signal_bars() const {
    if (no_signal_ || rx_count_ == 0)            return 0;
    const int q = quality_.bars(millis());
    if (q < 0)                                    return signal_bars_from_age();
    const int a = signal_bars_from_age();
    return (q < a) ? q : a;
}

void SlaveMode::draw_status_strip() {
    // Buffered paint session: the ~13 fill_rect + text ops that make
    // up this strip refresh batch into a single sprite, then push to
    // the panel as one SPI burst. Without this each op writes
    // independently to the panel and a panel scan-out crossing one
    // of those windows shows tear lines between elements.
    auto* ld = dal::local_driver_instance();
    const bool buffered =
        ld->begin_buffered_paint(0, 0, 240, kStripHeight);

    // Wipe the strip black so we can repaint icons cleanly without
    // residue from whatever was there last refresh.
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        0, 0, 240, kStripHeight, BLACK});

    // Mode label, left.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        2, 2, no_signal_ ? "NO SIG" : "Slave",
        no_signal_ ? RED : WHITE, BLACK, 1});

    // Signal bars: 4 vertical bars at heights 2/4/6/8 px, 2 px wide
    // each, lit count = signal_bars. Anchored at the right of the
    // strip just inside the battery icon. Bar count comes from the
    // sequence-loss-rate quality tracker (transport::SignalQuality)
    // with a frame-age fallback for cold start.
    const int sig_x   = 198;   // top-left of the signal-bars region
    const int sig_top = 2;
    const int bars    = signal_bars();
    for (int i = 0; i < 4; ++i) {
        const int bar_h = 2 + i * 2;          // 2,4,6,8
        const int bar_y = sig_top + (8 - bar_h);
        const uint16_t color = (i < bars) ? GREEN : 0x4208;  // dim grey for unlit
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            sig_x + i * 4, bar_y, 2, bar_h, color});
    }

    // Battery icon on the right: 24 x 8 outline + tip + filled
    // proportion.
    const int batt_x = 214;
    const int batt_y = 2;
    const int batt_w = 22;
    const int batt_h = 8;
    // Outline (top, bottom, left, right via 1-px fill_rects).
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

// Diagnostic body shown only while NO SIGNAL is active. The pulse-
// rect area below the strip is otherwise black (no incoming pulses)
// so we have the whole screen below the strip to spend on text.
void SlaveMode::draw_no_signal_body() {
    char line[40];

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 30, "NO SIGNAL", RED, BLACK, 3});

    std::snprintf(line, sizeof(line), "ch %u %s%s",
                  (unsigned)current_listen_chan_,
                  radio_active_ ? "listening" : "off",
                  slave_channel_pref_ == 0 ? " (scan)" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 70, line, WHITE, BLACK, 1});

    std::snprintf(line, sizeof(line), "rx total: %lu",
                  (unsigned long)rx_count_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 84, line, WHITE, BLACK, 1});

    if (rx_count_ > 0) {
        const uint32_t now = millis();
        const uint32_t age = (now >= last_rx_ms_) ? (now - last_rx_ms_) : 0;
        std::snprintf(line, sizeof(line), "last rx: %lu ms ago    ",
                      (unsigned long)age);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 98, line, RED, BLACK, 1});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B-hold: menu", WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
