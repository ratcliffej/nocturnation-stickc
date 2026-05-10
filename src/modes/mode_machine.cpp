// NocturNation Mode FSM implementation.
//
// All concrete mode classes live here for now. They are small enough
// individually that splitting into one file per mode would create more
// navigation cost than it saves. If any single mode grows past ~150 lines,
// lift it into its own file.
//
// NVS persistence (last-used runtime mode) is wrapped behind two helpers
// at the top so native test builds (no Arduino runtime, no Preferences
// library) compile without needing a mock.

#include "modes/mode_machine.h"
#include "effects/effects.h"
#include "hal/hal.h"
#include "transport/espnow/frame.h"
#include "transport/quality.h"             // SignalQuality
#include "../dal/drivers/local_driver.h"   // for set_pulse_enabled gating

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#else
// Native test stubs. millis() is needed by the Boot countdown; tests advance
// it explicitly via the seam below.
#include <cstdint>
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
namespace nocturnation {
namespace modes {
namespace test_seam {
void set_millis(uint32_t v) { s_native_millis = v; }
}  // namespace test_seam
}  // namespace modes
}  // namespace nocturnation
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

// =============================================================================
// NVS persistence (Arduino only)
// =============================================================================

namespace {

constexpr ModeId kDefaultRuntimeMode = ModeId::AutonomousMaster;

bool is_persisted_runtime_mode(ModeId m) {
    return m == ModeId::AutonomousMaster
        || m == ModeId::Slave
        || m == ModeId::Config
        || m == ModeId::Test;
}

// Audio-Live calibration. Per-band log2 floor/ceiling for the spectrum
// bars, plus an auto-calibrate flag. Defaults below are tuned from
// Jason's StickC Plus2 + Vengaboys reference; sound-check overrides them with
// per-device values, and auto mode bypasses these in favour of rolling
// min/max during AudioLive use (room/audience-adaptive).
struct AudioCalibration {
    float floor[4];        // B, M, T, R log2 floor
    float ceil [4];        // B, M, T, R log2 ceiling
    bool  auto_enabled;
};

constexpr AudioCalibration kCalibrationDefault = {
    /*floor=*/ { 14.0f, 14.0f, 15.0f,  5.0f },
    /*ceil =*/ { 19.0f, 20.0f, 21.0f, 10.0f },
    /*auto =*/ false,
};

AudioCalibration s_calibration = kCalibrationDefault;

#ifdef ARDUINO
ModeId load_last_runtime_mode() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t raw = prefs.getUChar("last_mode", (uint8_t)kDefaultRuntimeMode);
    prefs.end();
    ModeId m = (ModeId)raw;
    return is_persisted_runtime_mode(m) ? m : kDefaultRuntimeMode;
}

void save_last_runtime_mode(ModeId m) {
    if (!is_persisted_runtime_mode(m)) return;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("last_mode", (uint8_t)m);
    prefs.end();
}

bool load_ir_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("ir_en", true);     // default ON
    prefs.end();
    return e;
}

void save_ir_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("ir_en", e);
    prefs.end();
}

bool load_screen_pulse_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("scr_puls_en", true);   // default ON
    prefs.end();
    return e;
}

void save_screen_pulse_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("scr_puls_en", e);
    prefs.end();
}

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

void save_slave_ir_group(uint8_t g) {
    if (g > 5) g = 0;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_ir_grp", g);
    prefs.end();
}

// ESP-NOW radio channel preferences. Master uses one of {1, 6, 11}; slave
// uses {0=Auto/scan, 1, 6, 11}. Defaults: master 1 (hobby), slave 0 (auto-
// scan with show priority). Per architecture spec §4.5: channel 1 = hobby /
// open community traffic, channel 11 = show / commercial; channel 6 is an
// advanced operator override only.
uint8_t load_master_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("mst_chan", 1);
    prefs.end();
    if (c != 1 && c != 6 && c != 11) c = 1;
    return c;
}

void save_master_channel(uint8_t c) {
    if (c != 1 && c != 6 && c != 11) c = 1;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_chan", c);
    prefs.end();
}

uint8_t load_slave_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("slv_chan", 0);   // 0 = auto/scan
    prefs.end();
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    return c;
}

void save_slave_channel(uint8_t c) {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_chan", c);
    prefs.end();
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

void save_slave_repeat_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("slv_repeat", e);
    prefs.end();
}

AudioCalibration load_calibration() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    AudioCalibration c = kCalibrationDefault;
    if (prefs.getBytesLength("cal") == sizeof(AudioCalibration)) {
        prefs.getBytes("cal", &c, sizeof(AudioCalibration));
    }
    prefs.end();
    return c;
}

void save_calibration(const AudioCalibration& c) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBytes("cal", &c, sizeof(AudioCalibration));
    prefs.end();
}
#else
ModeId           load_last_runtime_mode() { return kDefaultRuntimeMode; }
void             save_last_runtime_mode(ModeId)  {}
bool             load_ir_enabled()             { return true; }
void             save_ir_enabled(bool)         {}
bool             load_screen_pulse_enabled()    { return true; }
void             save_screen_pulse_enabled(bool) {}
uint8_t          load_slave_ir_group()          { return 0; }
void             save_slave_ir_group(uint8_t)  {}
uint8_t          load_master_channel()          { return 1; }
void             save_master_channel(uint8_t) {}
uint8_t          load_slave_channel()           { return 0; }
void             save_slave_channel(uint8_t)  {}
bool             load_slave_repeat_enabled()    { return false; }
void             save_slave_repeat_enabled(bool) {}
AudioCalibration load_calibration()       { return kCalibrationDefault; }
void             save_calibration(const AudioCalibration&) {}
#endif

// =============================================================================
// EspNowBroadcaster - shared by AutonomousMaster and TestMode (and any future
// mode that wants to push frames over ESP-NOW). Each instance holds its own
// source_id / seq_num / radio-active state and calls HAL::esp_now() under
// the hood. begin/end follow the owning mode's lifecycle; HAL::esp_now()->
// begin() is idempotent so concurrent owners across mode transitions work
// fine.
//
// Stepping stone toward a proper EspNowDriver registered with the DAL behind
// render_fx("esp-now-broadcast", ev). When that lands, per-mode lifecycle
// goes away and the radio sits up for the firmware's life. Until then this
// keeps the broadcast logic in one place rather than duplicated across modes.
// =============================================================================

struct EspNowBroadcaster {
    static constexpr uint32_t kHeartbeatPeriodMs = 1000;   // 1 Hz alive signal

    // Per spec §4.3 reliability strategy: each frame goes out 3 times
    // total (initial + 2 retransmits) with the SAME sequence number,
    // separated by 5-15 ms of pseudo-random jitter. Slave dedup catches
    // the duplicates; the redundancy buys airtime resilience against
    // collisions and brief interference. Total send burst is ~20-30 ms,
    // well under the inter-beat interval at any musical tempo.
    static constexpr uint8_t  kRedundantSends     = 3;
    static constexpr uint8_t  kRedundantGapMinMs  = 5;
    static constexpr uint8_t  kRedundantGapMaxMs  = 15;

    // Maximum frame size we ever buffer for retransmit. Matches the
    // transport-level cap so the LIGHT_COMMAND frame (largest at 14
    // bytes including header) fits comfortably.
    static constexpr size_t   kRetransmitBufSize  = 32;

    bool      active_      = false;
    uint8_t   source_id_   = 1;
    uint8_t   seq_num_     = 1;
    uint32_t  last_tx_ms_  = 0;

    // Pending-retransmit state. When a frame is sent for the first time
    // we copy its bytes here and schedule the next 2 sends; pump_retransmits()
    // (called from the owning mode's loop_tick) drains them in order.
    uint8_t   retransmit_buf_[kRetransmitBufSize] = {};
    size_t    retransmit_len_       = 0;
    uint8_t   retransmits_remaining_ = 0;
    uint32_t  next_retransmit_ms_   = 0;

    bool begin(uint8_t channel) {
        if (active_) return true;
        auto* radio = hal::HAL::esp_now();
        if (!radio) return false;
        source_id_  = derive_source_id();
        seq_num_    = 1;
        last_tx_ms_ = 0;
        active_ = radio->begin(channel);
#ifdef ARDUINO
        if (!active_) {
            Serial.println("[espnow] broadcaster begin() failed");
        } else {
            Serial.printf("[espnow] broadcaster up: ch=%u src_id=%u\n",
                          (unsigned)channel, (unsigned)source_id_);
        }
#endif
        return active_;
    }

    void end() {
        if (!active_) return;
        if (auto* radio = hal::HAL::esp_now()) radio->end();
        active_ = false;
    }

    static uint8_t derive_source_id() {
#ifdef ARDUINO
        uint8_t mac[6] = {0};
        WiFi.macAddress(mac);
        uint8_t id = mac[5];
        if (id == 0 || id == 0xFF) id = (mac[4] != 0 && mac[4] != 0xFF) ? mac[4] : 1;
        return id;
#else
        return 1;
#endif
    }

    uint8_t next_seq() {
        const uint8_t s = seq_num_;
        seq_num_ = (seq_num_ == 255) ? 1 : (seq_num_ + 1);
        return s;
    }

    void send_frame_bytes(const uint8_t* buf, size_t n, const char* label) {
        if (!active_ || n == 0) return;
        auto* radio = hal::HAL::esp_now();
        if (!radio) return;
        const bool ok = radio->send_broadcast(buf, n);
        if (ok) last_tx_ms_ = millis();
#ifdef ARDUINO
        Serial.printf("[espnow TX %s%s] ", label, ok ? "" : " FAIL");
        for (size_t i = 0; i < n; ++i) Serial.printf("%02X ", buf[i]);
        Serial.println();
#else
        (void)ok; (void)label;
#endif

        // Schedule the redundant retransmits per spec §4.3. New frame
        // replaces any pending retransmit queue - if a beat lands while
        // a heartbeat is still mid-burst, we'd rather get the beat out
        // than complete the heartbeat's redundancy.
        if (n <= kRetransmitBufSize) {
            std::memcpy(retransmit_buf_, buf, n);
            retransmit_len_        = n;
            retransmits_remaining_ = kRedundantSends - 1;
            next_retransmit_ms_    = millis() + redundant_gap_ms();
        } else {
            retransmits_remaining_ = 0;
        }
    }

    // Drain any pending retransmits whose time has come. Called from the
    // owning mode's loop_tick (so we run on the main task, never the
    // ESP-NOW callback).
    void pump_retransmits() {
        if (!active_ || retransmits_remaining_ == 0) return;
        const uint32_t now = millis();
        if (now < next_retransmit_ms_) return;

        auto* radio = hal::HAL::esp_now();
        if (!radio) {
            retransmits_remaining_ = 0;
            return;
        }
        radio->send_broadcast(retransmit_buf_, retransmit_len_);
        last_tx_ms_ = now;
        retransmits_remaining_--;
        if (retransmits_remaining_ > 0) {
            next_retransmit_ms_ = now + redundant_gap_ms();
        }
    }

    static uint32_t redundant_gap_ms() {
        // Pseudo-random jitter in [kRedundantGapMinMs, kRedundantGapMaxMs].
        // std::rand() is good enough - we want spread, not cryptographic
        // unpredictability.
        const uint32_t span = kRedundantGapMaxMs - kRedundantGapMinMs + 1;
        return kRedundantGapMinMs + (std::rand() % span);
    }

    void send_beat(float strength_rms, float bpm) {
        if (!active_) return;
        using namespace transport::espnow;
        Header h{};
        h.source_id       = source_id_;
        h.sequence_number = next_seq();
        h.hop_count       = 0;
        BeatDetectedPayload p{};
        const float scaled = strength_rms / 20.0f;
        p.strength = (scaled < 0.0f)   ? 0
                   : (scaled > 255.0f) ? 255
                                       : static_cast<uint8_t>(scaled);
        const float bpm_x10 = bpm * 10.0f;
        p.bpm_x10 = (bpm_x10 < 0.0f)     ? 0
                  : (bpm_x10 > 65535.0f) ? 65535
                                         : static_cast<uint16_t>(bpm_x10);
        uint8_t buf[kHeaderSize + kBeatDetectedPayloadLen];
        const size_t n = encode_beat_detected(buf, sizeof(buf), h, p);
        send_frame_bytes(buf, n, "BEAT");
    }

    void send_heartbeat() {
        if (!active_) return;
        using namespace transport::espnow;
        Header h{};
        h.source_id       = source_id_;
        h.sequence_number = next_seq();
        h.hop_count       = 0;
        uint8_t buf[kHeaderSize + kHeartbeatPayloadLen];
        const size_t n = encode_heartbeat(buf, sizeof(buf), h);
        send_frame_bytes(buf, n, "HBEAT");
    }

    void send_light_command(uint8_t target_group,
                            uint8_t r, uint8_t g, uint8_t b,
                            effects::PulseEnvelope env,
                            pixmob::Chance chance) {
        if (!active_) return;
        using namespace transport::espnow;
        Header h{};
        h.source_id       = source_id_;
        h.sequence_number = next_seq();
        h.hop_count       = 0;
        LightCommandPayload p{};
        p.target_group = target_group;
        p.r = r; p.g = g; p.b = b;
        p.attack  = static_cast<uint8_t>(env.attack);
        p.sustain = static_cast<uint8_t>(env.sustain);
        p.release = static_cast<uint8_t>(env.release);
        p.chance  = static_cast<uint8_t>(chance);
        uint8_t buf[kHeaderSize + kLightCommandPayloadLen];
        const size_t n = encode_light_command(buf, sizeof(buf), h, p);
        send_frame_bytes(buf, n, "LIGHT");
    }

    // Convenience: encode + send LIGHT_COMMAND from an RgbPulseEvent. Used
    // by Test mode test fires - same RgbPulseEvent that drives the local
    // screen + IR also hits the wire so slaves render in sync.
    void send_light_command(uint8_t target_group, const RgbPulseEvent& ev) {
        send_light_command(target_group, ev.r, ev.g, ev.b,
                           effects::PulseEnvelope{ev.attack, ev.sustain, ev.release},
                           ev.chance);
    }

    // Master loop_tick calls this every iteration. If no frame has gone
    // out within kHeartbeatPeriodMs, sends one. During music with
    // BEAT_DETECTED firing every 350-500 ms, this short-circuits and
    // heartbeat traffic stays at zero.
    bool maybe_send_heartbeat() {
        if (!active_) return false;
        const uint32_t now = millis();
        if (now - last_tx_ms_ < kHeartbeatPeriodMs) return false;
        send_heartbeat();
        return true;
    }
};

}  // namespace

// =============================================================================
// Forward declarations of concrete mode classes (defined further down)
// =============================================================================

class BootMode;
class MenuMode;
class AutonomousMasterMode;
class SlaveMode;
class ConfigMode;
class TestMode;

// =============================================================================
// Internal state - active mode pointer + per-mode singletons live near the
// concrete classes (after their definitions). The FSM accessor functions
// at the bottom of this file consult this state.
// =============================================================================

namespace {

Mode*  s_active_mode      = nullptr;
ModeId s_last_runtime     = kDefaultRuntimeMode;

Mode* mode_instance(ModeId id);

void enter_mode(ModeId id) {
    Mode* next = mode_instance(id);
    if (!next) return;
    if (s_active_mode == next) return;
    if (s_active_mode) s_active_mode->exit();
    s_active_mode = next;
    if (is_persisted_runtime_mode(id)) {
        s_last_runtime = id;
        save_last_runtime_mode(id);
    }
    s_active_mode->enter();
}

}  // namespace

// =============================================================================
// Shared colour palette
// =============================================================================
//
// Used by AutonomousMasterMode (preserves Epic 1's six-state cycle including
// the OFF "skip IR" state) and TestMode (uses a five-state subset; no OFF
// because hitting "send a test pulse" with OFF selected would be confusing).

namespace {

enum class Colour : uint8_t {
    Off = 0, Red, Green, Blue, Yellow, White
};

const char* colour_name(Colour c) {
    switch (c) {
        case Colour::Off:    return "OFF";
        case Colour::Red:    return "RED";
        case Colour::Green:  return "GREEN";
        case Colour::Blue:   return "BLUE";
        case Colour::Yellow: return "YELLOW";
        case Colour::White:  return "WHITE";
    }
    return "?";
}

uint16_t colour_screen_rgb(Colour c) {
    switch (c) {
        case Colour::Red:    return RED;
        case Colour::Green:  return GREEN;
        case Colour::Blue:   return BLUE;
        case Colour::Yellow: return YELLOW;
        case Colour::White:  return WHITE;
        case Colour::Off:    return BLACK;
    }
    return BLACK;
}

void colour_to_rgb(Colour c, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (c) {
        case Colour::Red:    r=0xFF; g=0x00; b=0x00; break;
        case Colour::Green:  r=0x00; g=0xFF; b=0x00; break;
        case Colour::Blue:   r=0x00; g=0x00; b=0xFF; break;
        case Colour::Yellow: r=0xFF; g=0xFF; b=0x00; break;
        case Colour::White:  r=0xFF; g=0xFF; b=0xFF; break;
        case Colour::Off:    r=0;    g=0;    b=0;    break;
    }
}

}  // namespace

// =============================================================================
// BootMode - 5s countdown screen, button-press interrupt
// =============================================================================

namespace {

constexpr uint32_t kBootCountdownMs = 5000;

}

class BootMode : public Mode {
public:
    ModeId id() const override { return ModeId::Boot; }
    const char* name() const override { return "Boot"; }

    void enter() override {
        start_ms_           = millis();
        last_drawn_seconds_ = 0xFF;
        last_drawn_pulse_   = 0xFF;
        draw_static();
    }

    void loop_tick() override {
        const uint32_t now     = millis();
        const uint32_t elapsed = now - start_ms_;
        if (elapsed >= kBootCountdownMs) {
            ModeMachine::switch_to(s_last_runtime);
            return;
        }
        const uint8_t remaining = (uint8_t)((kBootCountdownMs - elapsed) / 1000) + 1;
        if (remaining != last_drawn_seconds_) {
            last_drawn_seconds_ = remaining;
            draw_countdown(remaining);
        }
        // Pulse the brand-mark N. 16 phases per cycle * 120 ms each gives
        // a ~2 s breathe. Anchor to start_ms_ so phase 0 (full brightness)
        // is always the first frame after entering Boot - the splash opens
        // bright instead of mid-cycle. Redraw only the single character
        // cell so there's no flicker on the rest of the splash.
        const uint8_t step = (uint8_t)(((now - start_ms_) / 120) & 0x0F);
        if (step != last_drawn_pulse_) {
            last_drawn_pulse_ = step;
            draw_pulsing_n(step);
        }
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (ev.kind == ButtonEvent::Pressed || ev.kind == ButtonEvent::Clicked) {
            ModeMachine::switch_to(ModeId::Menu);
        }
    }

private:
    uint32_t start_ms_           = 0;
    uint8_t  last_drawn_seconds_ = 0xFF;
    uint8_t  last_drawn_pulse_   = 0xFF;

    // Layout. Title is "Noctur" + pulsing "N" + "ation" at size 3 so the
    // brand mark matches the website banner. Size-3 char cells are 18 px
    // wide; 12 chars * 18 = 216 px, leaves ~12 px margin on each side of
    // the 240 px display.
    static constexpr int kTitleX  = 12;
    static constexpr int kTitleY  = 20;
    static constexpr int kCharW3  = 18;
    static constexpr int kPulseNX = kTitleX + 6 * kCharW3;   // start of 2nd N

    // Cosine-shaped brightness ramp over 16 phases, scaled to [178, 255]
    // - peaks at 255 (full bright) on phase 0 and dips ~30% to 178 on
    // phase 8. The N stays clearly visible the whole cycle; it never
    // dims to black. Orange/yellow tone (full red, ~half green, no blue).
    static uint16_t pulse_color(uint8_t step) {
        static constexpr uint8_t kBrightness[16] = {
            255, 252, 244, 231, 217, 202, 189, 181,
            178, 181, 189, 202, 217, 231, 244, 252,
        };
        const uint8_t b  = kBrightness[step & 0x0F];
        const uint8_t r5 = b >> 3;                          // 0..31
        const uint8_t g6 = ((uint16_t)b * 50 / 100) >> 2;   // ~half intensity
        return ((uint16_t)r5 << 11) | ((uint16_t)g6 << 5);
    }

    static const char* mode_label(ModeId m) {
        switch (m) {
            case ModeId::AutonomousMaster: return "Master";
            case ModeId::Slave:            return "Slave";
            case ModeId::Config:           return "Config";
            case ModeId::Test:             return "Test";
            default:                       return "?";
        }
    }

    void draw_static() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        // Brand title. "Noctur" + (pulsing N drawn separately) + "ation".
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kTitleX, kTitleY, "Noctur", WHITE, BLACK, 3});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kTitleX + 7 * kCharW3, kTitleY, "ation", WHITE, BLACK, 3});

        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kTitleX, kTitleY + 28, "Open-source crowd lighting.",
            WHITE, BLACK, 1});

        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kTitleX, 115, "press any btn for menu", WHITE, BLACK, 1});
    }

    void draw_countdown(uint8_t seconds) {
        // Fixed-width format ("X in N s" - one digit) so subsequent draws
        // overwrite the previous text cell-for-cell with no flicker.
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s in %u s",
                      mode_label(s_last_runtime), (unsigned)seconds);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kTitleX, 75, buf, WHITE, BLACK, 2});
    }

    void draw_pulsing_n(uint8_t step) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            kPulseNX, kTitleY, "N", pulse_color(step), BLACK, 3});
    }
};

// =============================================================================
// MenuMode - Btn2 cycles, Btn1 selects
// =============================================================================

namespace {

struct MenuItem {
    ModeId      target;
    const char* label;
};

constexpr MenuItem kMenuItems[] = {
    { ModeId::AutonomousMaster, "Autonomous Master" },
    { ModeId::Slave,            "Slave"             },
    { ModeId::Test,             "Test"              },
    { ModeId::Config,           "Config"            },
};
constexpr size_t kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);

}

class MenuMode : public Mode {
public:
    ModeId id() const override { return ModeId::Menu; }
    const char* name() const override { return "Menu"; }

    void enter() override {
        // Default the cursor to whichever item is the persisted last-used
        // runtime mode, so a quick Btn1 press confirms the previous choice.
        selected_ = 0;
        for (size_t i = 0; i < kMenuItemCount; ++i) {
            if (kMenuItems[i].target == s_last_runtime) {
                selected_ = i;
                break;
            }
        }
        draw();
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (ev.kind != ButtonEvent::Pressed) return;
        if (ev.id == ButtonId::Btn2) {
            selected_ = (selected_ + 1) % kMenuItemCount;
            draw();
        } else if (ev.id == ButtonId::Btn1) {
            ModeMachine::switch_to(kMenuItems[selected_].target);
        }
    }

private:
    size_t selected_ = 0;

    void draw() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Mode", WHITE, BLACK, 3});
        for (size_t i = 0; i < kMenuItemCount; ++i) {
            const bool sel = (i == selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", kMenuItems[i].label);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 45 + (int)i * 18, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B: cycle  A: select", WHITE, BLACK, 1});
    }
};

// =============================================================================
// AutonomousMasterMode - lifts the Epic 1 beat-detection + IR-sending logic
// out of main.cpp. Behaviour is preserved exactly; the only change is that
// PWR long-press now goes to the Menu instead of toggling beat-mode.
// =============================================================================

namespace {

constexpr float         kBaselineAlpha     = 0.02f;
constexpr float         kBeatMultiplier    = 2.5f;
constexpr float         kFluxFloor         = 2000.0f;
constexpr uint32_t      kBeatRefractoryMs  = 200;
constexpr float         kVolumeGate        = 500.0f;
constexpr size_t        kIbiBufferSize     = 8;

}

class AutonomousMasterMode : public Mode {
public:
    ModeId id() const override { return ModeId::AutonomousMaster; }
    const char* name() const override { return "Autonomous Master"; }

    AutonomousMasterMode() : pulse_("all-pixmobs") {}

    void enter() override {
        // Reset beat-detection state; re-enable mic input via the DAL.
        baseline_flux_     = 100.0f;
        prev_bass_energy_  = 0.0f;
        current_flux_      = 0.0f;
        ibi_index_         = 0;
        ibi_count_         = 0;
        estimated_bpm_     = 0.0f;
        last_beat_ms_      = 0;
        last_draw_ms_      = 0;
        paused_            = false;
        sync_pulse_colour();
        pulse_.enter();
        DAL::start_audio_input("local", 16000, 512);
        // Channel from NVS (Config > ESP-NOW > Master Channel). Default
        // 1 (hobby); show deployments configure 11; 6 is an advanced
        // operator override.
        broadcaster_.begin(load_master_channel());
        draw();
    }

    void exit() override {
        DAL::stop_audio_input("local");
        broadcaster_.end();
        pulse_.exit();
    }

    void loop_tick() override {
        const uint32_t now = millis();
        if (now - last_draw_ms_ > 50) {
            draw();
            last_draw_ms_ = now;
        }
        // Drain any pending redundant retransmits (per spec §4.3).
        broadcaster_.pump_retransmits();
        // Skip-if-recent heartbeat: keep master-alive flowing only when
        // BEAT_DETECTED traffic isn't already covering it.
        broadcaster_.maybe_send_heartbeat();
    }

    void on_audio_frame(const AudioFrameEvent& ev) override {
        current_level_ = ev.overall_rms;

        if (current_level_ < kVolumeGate) {
            prev_bass_energy_ = 0.0f;
            return;
        }

        // Track flux + baseline for the audio meter display only -
        // beat firing is now decided by the DAL analyser's
        // BeatDetector which consumes the 32-band spectrum frame
        // (Epic 4.5 Block 3). Self-calibrating threshold per sub-band
        // means the same ev.is_beat semantics hold across Plus2 and S3
        // regardless of mic SNR. The flux/baseline values are still
        // useful as a "what the old single-threshold detector would
        // have seen" diagnostic on the meter strip; they have no
        // bearing on which frames fire beats.
        float flux = ev.bass_energy - prev_bass_energy_;
        if (flux < 0) flux = 0;
        prev_bass_energy_ = ev.bass_energy;
        current_flux_     = flux;

        baseline_flux_ = baseline_flux_ * (1.0f - kBaselineAlpha)
                       + flux * kBaselineAlpha;

        const uint32_t now = millis();
        if (!ev.is_beat) return;

        // BPM tracking: record IBI before updating last_beat_ms_.
        if (last_beat_ms_ > 0) {
            const uint32_t ibi = now - last_beat_ms_;
            if (ibi >= 300 && ibi <= 1200) {
                ibi_buffer_[ibi_index_] = ibi;
                ibi_index_ = (ibi_index_ + 1) % kIbiBufferSize;
                if (ibi_count_ < kIbiBufferSize) ibi_count_++;
                update_bpm_from_buffer();
            }
        }
        last_beat_ms_ = now;

        // Broadcast to any slaves over ESP-NOW. Two frames per beat:
        //   - BEAT_DETECTED carries strength + BPM as metadata for any
        //     slave that wants to drive its own colour scheme (e.g. a
        //     constellation art piece) or display a BPM readout.
        //   - LIGHT_COMMAND carries the exact RGB + envelope the master's
        //     local IR fire used, so a slave that wants to be a literal
        //     "extra light" in the show can render the same colour with
        //     the same envelope on its screen.
        // send is async at the radio layer so this returns quickly; the
        // transmission overlaps with the local screen flash + IR fire
        // below. Skipped when paused so the entire deployment goes
        // silent on a single mute press; the periodic heartbeat keeps
        // slaves' master-loss detection from tripping during a pause.
        if (!paused_) {
            broadcaster_.send_beat(current_level_, estimated_bpm_);
            uint8_t r=0, g=0, b=0;
            colour_to_rgb(colour_, r, g, b);
            broadcaster_.send_light_command(
                /*target_group=*/0, r, g, b,
                effects::envelope_for_bpm(estimated_bpm_),
                pixmob::CHANCE_100);
        }

        // Beat response. Same ordering as the prototype: flash, fire IR (if
        // not muted), short delay, redraw. The IR firing now goes through
        // the Pulse effect, which owns the BPM->envelope mapping.
        DAL::fire_display_clear("local",
            DisplayClearEvent{colour_screen_rgb(colour_)});
        if (!paused_) {
            pulse_.on_beat(now, estimated_bpm_);
        }
        delay_ms(30);
        draw();
        last_draw_ms_ = millis();
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        // Btn2 (side): cycle colour.
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
            colour_ = (Colour)(((uint8_t)colour_ + 1) % 6);
            sync_pulse_colour();
            draw();
            return;
        }
        // Btn1 (front): mute toggle + test pulse.
        if (ev.id == ButtonId::Btn1 && ev.kind == ButtonEvent::Pressed) {
            paused_ = !paused_;
            pulse_.on_beat(millis(), estimated_bpm_);
            return;
        }
        // BtnB long-press: back to menu. Two-button UI on both Plus2 and
        // S3 - BtnPWR is hardware-managed on the S3 (PMIC owns reset/off),
        // so the firmware UI is consistently BtnA + BtnB across hosts.
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
            ModeMachine::switch_to(ModeId::Menu);
            return;
        }
    }

private:
    // Channel comes from NVS (Config > ESP-NOW > Master Channel) per
    // spec §4.5: 1 = hobby (default), 11 = show, 6 = advanced override.

    EspNowBroadcaster broadcaster_;

    Colour            colour_           = Colour::Red;
    bool              paused_           = false;
    effects::Pulse    pulse_;

    float     baseline_flux_    = 100.0f;
    float     prev_bass_energy_ = 0.0f;
    float     current_flux_     = 0.0f;
    float     current_level_    = 0.0f;
    uint32_t  last_beat_ms_     = 0;
    uint32_t  last_draw_ms_     = 0;

    uint32_t  ibi_buffer_[kIbiBufferSize] = {0};
    size_t    ibi_index_        = 0;
    size_t    ibi_count_        = 0;
    float     estimated_bpm_    = 0.0f;

    static void delay_ms(uint32_t ms) {
#ifdef ARDUINO
        ::delay(ms);
#else
        (void)ms;
#endif
    }

    void sync_pulse_colour() {
        uint8_t r=0, g=0, b=0;
        colour_to_rgb(colour_, r, g, b);
        pulse_.set_colour(r, g, b);
    }

    void update_bpm_from_buffer() {
        if (ibi_count_ < 3) return;
        uint32_t sorted[kIbiBufferSize];
        for (size_t i = 0; i < ibi_count_; ++i) sorted[i] = ibi_buffer_[i];
        for (size_t i = 1; i < ibi_count_; ++i) {
            uint32_t key = sorted[i];
            size_t j = i;
            while (j > 0 && sorted[j-1] > key) { sorted[j] = sorted[j-1]; --j; }
            sorted[j] = key;
        }
        const uint32_t med = (ibi_count_ % 2 == 1)
            ? sorted[ibi_count_ / 2]
            : (sorted[ibi_count_ / 2 - 1] + sorted[ibi_count_ / 2]) / 2;
        if (med > 50) estimated_bpm_ = 60000.0f / (float)med;
    }

    void draw() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

        char title[32];
        std::snprintf(title, sizeof(title), " %s%s",
                      colour_name(colour_), paused_ ? " : Muted" : "");
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, title, WHITE, BLACK, 3});

        char bpm[24];
        if (estimated_bpm_ > 0.0f) {
            std::snprintf(bpm, sizeof(bpm), " BPM: %.0f", estimated_bpm_);
        } else {
            std::snprintf(bpm, sizeof(bpm), " BPM: ---");
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 40, bpm, WHITE, BLACK, 2});

        // Batt + IR fire counter on one line. The IR count is 4-char max
        // (k/M suffix above 10000) so the whole line stays within 240 px
        // at size 2.
        const uint32_t ir = DAL::driver_send_count("ir-pixmob");
        char ir_buf[8];
        if      (ir >= 1000000) std::snprintf(ir_buf, sizeof(ir_buf), "%luM",
                                              (unsigned long)(ir / 1000000));
        else if (ir >=   10000) std::snprintf(ir_buf, sizeof(ir_buf), "%luk",
                                              (unsigned long)(ir / 1000));
        else                    std::snprintf(ir_buf, sizeof(ir_buf), "%lu",
                                              (unsigned long)ir);
        char batt[40];
        std::snprintf(batt, sizeof(batt), "Batt: %d%% IR: %s",
                      DAL::battery_level("local"), ir_buf);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 70, batt, WHITE, BLACK, 2});

        // Flux meter (frame + bar + threshold marker), composed from
        // FillRect primitives.
        const int meterX = 10, meterY = 110, meterW = 220, meterH = 14;
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            meterX, meterY,             meterW, 1,      WHITE});
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            meterX, meterY + meterH-1,  meterW, 1,      WHITE});
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            meterX, meterY,             1,      meterH, WHITE});
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            meterX + meterW-1, meterY,  1,      meterH, WHITE});

        const float ratio = (baseline_flux_ > 1.0f)
                                ? current_flux_ / baseline_flux_
                                : 0.0f;
        int barW = (int)(ratio * 50.0f);
        if (barW < 0)            barW = 0;
        if (barW > meterW - 2)   barW = meterW - 2;
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            meterX + 1, meterY + 1, barW, meterH - 2, GREEN});

        const int thrX = meterX + (int)(kBeatMultiplier * 50.0f);
        if (thrX < meterX + meterW) {
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                thrX, meterY - 2, 1, meterH + 4, RED});
        }
    }
};

// =============================================================================
// SlaveMode - ESP-NOW receive (Epic 4 Block 3, RX side).
//
// Pulls the radio up on channel 1 (matching the master's hobby default;
// channel-priority dual-scan lands in Block 6) and registers a receive
// callback. Each inbound frame is decoded into its typed payload and
// logged via Serial - this is the byte-for-byte verification path the
// Epic Block 3 acceptance criterion calls for.
//
// Higher-level orchestration (deduplication, master-loss timeout +
// idle-effect fallback, display-as-light, IR re-fire on the slave) is
// Block 4's work. For now SlaveMode just shows a counter of received
// frames and the last source_id / message type seen.
// =============================================================================

class SlaveMode : public Mode {
public:
    ModeId id() const override { return ModeId::Slave; }
    const char* name() const override { return "Slave"; }

    void enter() override {
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

    void exit() override {
        if (radio_active_) {
            if (auto* radio = hal::HAL::esp_now()) radio->end();
            radio_active_ = false;
        }
        // Restore full-screen pulse rect for whichever mode comes next.
        dal::local_driver_instance()->reset_pulse_rect();
    }

    void loop_tick() override {
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

    void on_button_event(const ButtonPressEvent& ev) override {
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
            ModeMachine::switch_to(ModeId::Menu);
        }
    }

private:
    // No-signal threshold: 3x the master's heartbeat period (1 Hz). Three
    // missed heartbeats with no other traffic = master almost certainly
    // gone. Slaves do NOT auto-promote to master on this transition - the
    // master might be momentarily out of range or paused, and a rogue
    // slave-promoted-to-master would compete with the real master and
    // ruin show coordination. Block 4 will run a subtle local idle
    // effect through this state; for now we just display NO SIGNAL.
    static constexpr uint32_t kNoSignalMs          = 3000;

    // Status strip (always-visible 12 px band at the top with battery +
    // signal-strength icons). Pulse rect is set to the area BELOW this
    // strip during enter() so pulses don't repaint over the icons.
    static constexpr int      kStripHeight         = 12;
    static constexpr uint32_t kStripRefreshMs      = 500;

    bool      radio_active_       = false;
    uint32_t  rx_count_           = 0;
    uint32_t  last_rx_ms_         = 0;
    uint32_t  last_draw_ms_       = 0;
    uint32_t  last_strip_draw_ms_ = 0;
    uint8_t   last_source_id_     = 0;
    uint8_t   last_msg_type_      = 0xFF;
    bool      no_signal_          = false;   // sticky once threshold crossed

    // Slave IR forward group (0=broadcast/all-pixmobs, 1..5=specific).
    // Loaded from NVS on enter() so the operator's choice survives reboot.
    uint8_t   slave_ir_group_     = 0;

    // Channel preference: 0 = auto (dual-channel scan with show priority),
    // 1 / 6 / 11 = locked to that channel. Loaded from NVS on enter().
    uint8_t   slave_channel_pref_   = 0;
    uint8_t   current_listen_chan_  = 1;
    uint32_t  last_chan_switch_ms_  = 0;
    static constexpr uint32_t kChannelDwellMs = 2000;

    // Sequence-loss-rate signal quality. Transport-agnostic; could feed
    // off any sequenced protocol (future BLE / IR ack channels) the same
    // way it does ESP-NOW today.
    transport::SignalQuality quality_;

    // Deferred LIGHT_COMMAND queue (single slot; new arrivals replace).
    // The ESP-NOW receive callback runs on the WiFi task; calling
    // IRsend::sendRaw (~30 ms blocking GPIO loop) from that context
    // crashes the chip. We copy the payload here in on_recv and drain
    // in loop_tick (main task context, safe for the IR send timing).
    volatile bool                            pending_light_ = false;
    transport::espnow::LightCommandPayload   pending_light_payload_{};

    // Repeater mode (per spec §4.3, configurable per-slave via
    // Config > ESP-NOW > Repeat). When enabled, each unique inbound
    // frame is rebroadcast once with hop_count incremented by 1, up
    // to a 3-hop ceiling. Source_id and sequence_number are preserved
    // exactly so dedup works across the mesh - other slaves receiving
    // both the original and the repeat see them as duplicates.
    //
    // Queue same shape as the LIGHT_COMMAND queue: copy in on_recv,
    // drain in loop_tick (off the WiFi callback context).
    bool          slave_repeat_en_      = false;
    volatile bool pending_repeat_       = false;
    size_t        pending_repeat_len_   = 0;
    static constexpr size_t kRepeatBufSize  = 32;
    static constexpr uint8_t kMaxHopCount   = 3;
    uint8_t       pending_repeat_buf_[kRepeatBufSize] = {};

    // Deduplication ring (architecture spec §4.3): receivers track the
    // last 16 (source_id, sequence_number) tuples and drop repeats. The
    // master sends each frame 2-3 times for airtime resilience (Block 5
    // adds the redundant TX); without this gate the slave would paint
    // and fire IR twice per logical beat.
    //
    // Sequence number 0 is reserved as "sequencing disabled" per spec -
    // we treat those frames as always fresh (never deduped). All other
    // values 1-255 wrap normally.
    //
    // Only the WiFi-task on_recv reads/writes this; loop_tick only
    // observes pending_light_ + pending_light_payload_.
    struct DedupEntry { uint8_t source_id; uint8_t sequence_number; };
    static constexpr size_t kDedupRingSize = 16;
    DedupEntry dedup_ring_[kDedupRingSize] = {};
    size_t     dedup_head_ = 0;

    bool seen_recently(uint8_t src, uint8_t seq) const {
        if (seq == 0) return false;
        for (size_t i = 0; i < kDedupRingSize; ++i) {
            if (dedup_ring_[i].source_id == src
             && dedup_ring_[i].sequence_number == seq) {
                return true;
            }
        }
        return false;
    }

    void mark_seen(uint8_t src, uint8_t seq) {
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

    void render_light(const transport::espnow::LightCommandPayload& p) {
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
    static const char* ir_target_name(uint8_t group_id) {
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

    void on_recv(const hal::ESPNowMessage& m) {
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
    int signal_bars_from_age() const {
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
    int signal_bars() const {
        if (no_signal_ || rx_count_ == 0)            return 0;
        const int q = quality_.bars(millis());
        if (q < 0)                                    return signal_bars_from_age();
        const int a = signal_bars_from_age();
        return (q < a) ? q : a;
    }

    void draw_status_strip() {
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
    void draw_no_signal_body() {
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
};

// =============================================================================
// ConfigMode - the §8.4 config tree.
//
// Two-level navigation: top-level submenu list -> submenu items. Btn2
// cycles selection at either level. Btn1 enters a submenu from the top
// level, and activates an item within a submenu (toggling, cycling, or
// triggering depending on item type). PWR-hold pops one level (submenu
// -> top, top -> mode menu).
//
// Pre-Epic-4 / pre-Epic-7 status: the menu *shape* is built out per spec
// §8.4 so users can see what config will exist. Functional leaves at
// this milestone live under System (firmware version, default boot mode
// info, factory reset, battery status). Audio / IR / ESP-NOW / WiFi /
// DMX submenus list their planned items as labels but are non-
// interactive; they fill in when the relevant transport / capability
// epics ship.
// =============================================================================

class ConfigMode : public Mode {
public:
    ModeId id() const override { return ModeId::Config; }
    const char* name() const override { return "Config"; }

    void enter() override {
        level_         = Level::Top;
        active_sub_    = SubMenu::None;
        top_selected_  = 0;
        sub_selected_  = 0;
        confirm_until_ms_ = 0;
        last_drawn_battery_ = -2;     // force first battery redraw
        draw();
    }

    void loop_tick() override {
        // System submenu has live read-outs (battery percent); refresh
        // every ~500 ms when shown. Confirmation flash for factory reset
        // also clears here.
        const uint32_t now = millis();
        if (confirm_until_ms_ != 0 && now >= confirm_until_ms_) {
            confirm_until_ms_ = 0;
            draw();
            return;
        }
        if (level_ == Level::Sub && active_sub_ == SubMenu::System) {
            const int batt = DAL::battery_level("local");
            if (batt != last_drawn_battery_) {
                last_drawn_battery_ = batt;
                draw();
            }
        }
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (ev.kind != ButtonEvent::Pressed
         && ev.kind != ButtonEvent::LongPressed) return;

        // BtnB long-press pops one level. PixMob's two-level structure
        // (menu -> SetGroupId/GroupTarget workflow) gets an extra pop step
        // before it returns to the Config top-level.
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
            if (level_ == Level::Top) {
                ModeMachine::switch_to(ModeId::Menu);
            } else if (active_sub_ == SubMenu::PixMob
                    && pixmob_state_ != PixMobState::Menu) {
                pixmob_state_     = PixMobState::Menu;
                confirm_until_ms_ = 0;
                draw();
            } else {
                level_      = Level::Top;
                active_sub_ = SubMenu::None;
                draw();
            }
            return;
        }

        if (ev.kind != ButtonEvent::Pressed) return;
        if (level_ == Level::Top) handle_top(ev);
        else                       handle_sub(ev);
    }

private:
    enum class Level   : uint8_t { Top, Sub };
    enum class SubMenu : uint8_t {
        None = 0, Audio, Display, IR, EspNow, WiFi, Dmx, PixMob, System,
    };

    // Within the PixMob submenu, drilling into one of its items enters a
    // workflow sub-state with its own button bindings. Menu = the list
    // (Set Group ID / Group Target); SetGroupId / GroupTarget = the two
    // workflow screens.
    enum class PixMobState : uint8_t { Menu, SetGroupId, GroupTarget };

    struct TopEntry {
        SubMenu     target;
        const char* label;
    };
    static constexpr TopEntry kTop[8] = {
        { SubMenu::Audio,   "Audio"   },
        { SubMenu::Display, "Display" },
        { SubMenu::IR,      "IR"      },
        { SubMenu::EspNow,  "ESP-NOW" },
        { SubMenu::WiFi,    "WiFi"    },
        { SubMenu::Dmx,     "DMX"     },
        { SubMenu::PixMob,  "PixMob"  },
        { SubMenu::System,  "System"  },
    };
    static constexpr size_t kTopCount = sizeof(kTop) / sizeof(kTop[0]);

    Level       level_              = Level::Top;
    SubMenu     active_sub_         = SubMenu::None;
    size_t      top_selected_       = 0;
    size_t      sub_selected_       = 0;
    uint32_t    confirm_until_ms_   = 0;
    int         last_drawn_battery_ = -2;

    PixMobState pixmob_state_       = PixMobState::Menu;
    size_t      pixmob_selected_    = 0;
    uint8_t     pixmob_target_group_ = 1;
    static constexpr size_t kPixMobItemCount = 2;
    static constexpr uint32_t kConfirmFlashMs = 800;     // "Sent!" linger

    // -------------------------------------------------------------------------
    // Top-level navigation
    // -------------------------------------------------------------------------

    void handle_top(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            top_selected_ = (top_selected_ + 1) % kTopCount;
            draw();
        } else if (ev.id == ButtonId::Btn1) {
            level_         = Level::Sub;
            active_sub_    = kTop[top_selected_].target;
            sub_selected_  = 0;
            last_drawn_battery_ = -2;
            // Fresh entry into PixMob always lands on the menu screen
            // (not in a previous workflow).
            pixmob_state_     = PixMobState::Menu;
            pixmob_selected_  = 0;
            confirm_until_ms_ = 0;
            draw();
        }
    }

    void draw_top() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Config", WHITE, BLACK, 2});
        for (size_t i = 0; i < kTopCount; ++i) {
            const bool sel = (i == top_selected_);
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", kTop[i].label);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 22 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: enter  PWR: back",
            WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Submenu dispatch
    // -------------------------------------------------------------------------

    void handle_sub(const ButtonPressEvent& ev) {
        switch (active_sub_) {
            case SubMenu::System:  handle_system(ev);  break;
            case SubMenu::IR:      handle_ir(ev);      break;
            case SubMenu::Display: handle_display(ev); break;
            case SubMenu::EspNow:  handle_espnow(ev);  break;
            case SubMenu::PixMob:  handle_pixmob(ev);  break;
            case SubMenu::Audio:
            case SubMenu::WiFi:
            case SubMenu::Dmx:
                // Stub submenus accept Btn2 cycling for read-only browsing
                // of their planned-items list, but Btn1 is a no-op until
                // the relevant Epic wires real behaviour in.
                if (ev.id == ButtonId::Btn2) {
                    sub_selected_ = (sub_selected_ + 1) % stub_item_count();
                    draw();
                }
                break;
            default: break;
        }
    }

    void draw_sub() {
        switch (active_sub_) {
            case SubMenu::Audio:   draw_stub("Audio", kAudioItems, kAudioItemCount, "Epic 3"); break;
            case SubMenu::Display: draw_display(); break;
            case SubMenu::IR:      draw_ir(); break;
            case SubMenu::EspNow:  draw_espnow(); break;
            case SubMenu::WiFi:   draw_stub("WiFi",    kWifiItems,   kWifiItemCount,   "Epic 4"); break;
            case SubMenu::Dmx:    draw_stub("DMX",     kDmxItems,    kDmxItemCount,    "Epic 7"); break;
            case SubMenu::PixMob: draw_pixmob(); break;
            case SubMenu::System: draw_system(); break;
            default: break;
        }
    }

    // -------------------------------------------------------------------------
    // Stub-submenu data (planned items per spec §8.4; non-interactive)
    // -------------------------------------------------------------------------

    static constexpr const char* kAudioItems[] = {
        "Enable / Disable", "Show FFT spectrum", "Show beat meter", "Tuning",
    };
    static constexpr size_t kAudioItemCount = sizeof(kAudioItems) / sizeof(kAudioItems[0]);

    static constexpr const char* kIrItems[] = {
        "Enable / Disable", "Protocol", "Group ID assignment",
    };
    static constexpr size_t kIrItemCount = sizeof(kIrItems) / sizeof(kIrItems[0]);

    static constexpr const char* kEspNowItems[] = {
        "Enable / Disable", "Channel number", "Source ID",
    };
    static constexpr size_t kEspNowItemCount = sizeof(kEspNowItems) / sizeof(kEspNowItems[0]);

    static constexpr const char* kWifiItems[] = {
        "Enable / Disable", "SSID", "Password", "Soft-AP mode",
    };
    static constexpr size_t kWifiItemCount = sizeof(kWifiItems) / sizeof(kWifiItems[0]);

    static constexpr const char* kDmxItems[] = {
        "Carrier", "Universe ID", "Channel mapping",
    };
    static constexpr size_t kDmxItemCount = sizeof(kDmxItems) / sizeof(kDmxItems[0]);

    size_t stub_item_count() const {
        switch (active_sub_) {
            case SubMenu::Audio:   return kAudioItemCount;
            case SubMenu::Display: return kDisplayFunctionalItemCount;
            case SubMenu::IR:      return kIrItemCount;
            case SubMenu::EspNow:  return kEspNowFunctionalItemCount;
            case SubMenu::WiFi:    return kWifiItemCount;
            case SubMenu::Dmx:     return kDmxItemCount;
            default:               return 1;
        }
    }

    void draw_stub(const char* title,
                   const char* const* items, size_t count,
                   const char* epic_tag) {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, title, WHITE, BLACK, 2});
        char banner[24];
        std::snprintf(banner, sizeof(banner), "TBD %s", epic_tag);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            150, 10, banner, YELLOW, BLACK, 1});

        for (size_t i = 0; i < count; ++i) {
            const bool sel = (i == sub_selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", items[i]);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  PWR: back",
            WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Display submenu (functional: Pulse Enable toggle + persists)
    //
    // Pulse Enable gates the LocalDriver's RgbPulse handler only - all
    // other Display* output (status text, menus, etc.) keeps working
    // when this is OFF. Useful when an operator wants the screen to
    // show diagnostics/UI but not flash on beats.
    // -------------------------------------------------------------------------

    enum class DisplayItem : uint8_t {
        PulseEnable = 0,
    };
    static constexpr size_t kDisplayFunctionalItemCount = 1;

    void handle_display(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            sub_selected_ = (sub_selected_ + 1) % kDisplayFunctionalItemCount;
            draw();
            return;
        }
        if (ev.id == ButtonId::Btn1
         && (DisplayItem)sub_selected_ == DisplayItem::PulseEnable) {
            const bool next = !dal::local_driver_instance()->pulse_enabled();
            dal::local_driver_instance()->set_pulse_enabled(next);
            save_screen_pulse_enabled(next);
            draw();
        }
    }

    void draw_display() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Display", WHITE, BLACK, 2});

        char ena[24];
        std::snprintf(ena, sizeof(ena), "Pulse: %s",
                      dal::local_driver_instance()->pulse_enabled()
                          ? "ON" : "OFF");
        const char* lines[kDisplayFunctionalItemCount] = { ena };

        for (size_t i = 0; i < kDisplayFunctionalItemCount; ++i) {
            const bool sel = (i == sub_selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", lines[i]);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "A: toggle  B-hold: back",
            WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // IR submenu (functional: Enable toggles + persists; Protocol/GroupID info)
    // -------------------------------------------------------------------------

    enum class IRItem : uint8_t {
        EnableDisable = 0,
        Protocol,
        SlaveGroup,
    };
    static constexpr size_t kIrFunctionalItemCount = 3;

    void handle_ir(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            sub_selected_ = (sub_selected_ + 1) % kIrFunctionalItemCount;
            draw();
            return;
        }
        if (ev.id == ButtonId::Btn1) {
            if ((IRItem)sub_selected_ == IRItem::EnableDisable) {
                const bool next = !DAL::driver_enabled("ir-pixmob");
                DAL::set_driver_enabled("ir-pixmob", next);
                save_ir_enabled(next);
                draw();
            } else if ((IRItem)sub_selected_ == IRItem::SlaveGroup) {
                // Cycle 0 (broadcast / all-pixmobs) -> 1 .. 5 -> 0.
                uint8_t g = load_slave_ir_group();
                g = (g + 1) % 6;
                save_slave_ir_group(g);
                draw();
            }
        }
        // Protocol is info-only - no Btn1 action.
    }

    void draw_ir() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "IR", WHITE, BLACK, 2});

        char ena[24];
        std::snprintf(ena, sizeof(ena), "Enable: %s",
                      DAL::driver_enabled("ir-pixmob") ? "ON" : "OFF");
        char grp[24];
        const uint8_t cur_grp = load_slave_ir_group();
        if (cur_grp == 0) {
            std::snprintf(grp, sizeof(grp), "Slave Grp: all");
        } else {
            std::snprintf(grp, sizeof(grp), "Slave Grp: %u", (unsigned)cur_grp);
        }
        const char* lines[kIrFunctionalItemCount] = {
            ena,
            "Protocol: PixMob",
            grp,
        };

        for (size_t i = 0; i < kIrFunctionalItemCount; ++i) {
            const bool sel = (i == sub_selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", lines[i]);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: act  B-hold: back",
            WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // ESP-NOW submenu (functional: Master Channel + Slave Channel)
    //
    // Per architecture spec §4.5 the project's two-channel social contract
    // is channel 1 = hobby/community/open, channel 11 = show/commercial.
    // Master picks 1, 11, or 6 (advanced override). Slave picks Auto (dual-
    // channel scan with show priority) or locks to a specific channel.
    // Both persist to NVS and survive reboot.
    // -------------------------------------------------------------------------

    enum class EspNowItem : uint8_t {
        MasterChannel = 0,
        SlaveChannel,
        SlaveRepeat,
    };
    static constexpr size_t kEspNowFunctionalItemCount = 3;

    static const char* master_channel_label(uint8_t c) {
        switch (c) {
            case 1:  return "1 hobby";
            case 6:  return "6 custom";
            case 11: return "11 show";
            default: return "1 hobby";
        }
    }

    static const char* slave_channel_label(uint8_t c) {
        switch (c) {
            case 0:  return "auto scan";
            case 1:  return "1 hobby";
            case 6:  return "6 custom";
            case 11: return "11 show";
            default: return "auto scan";
        }
    }

    static uint8_t cycle_master_channel(uint8_t c) {
        // 1 -> 6 -> 11 -> 1
        switch (c) {
            case 1:  return 6;
            case 6:  return 11;
            case 11: return 1;
            default: return 1;
        }
    }

    static uint8_t cycle_slave_channel(uint8_t c) {
        // 0 (auto) -> 1 -> 6 -> 11 -> 0
        switch (c) {
            case 0:  return 1;
            case 1:  return 6;
            case 6:  return 11;
            case 11: return 0;
            default: return 0;
        }
    }

    void handle_espnow(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            sub_selected_ = (sub_selected_ + 1) % kEspNowFunctionalItemCount;
            draw();
            return;
        }
        if (ev.id == ButtonId::Btn1) {
            switch ((EspNowItem)sub_selected_) {
                case EspNowItem::MasterChannel:
                    save_master_channel(cycle_master_channel(load_master_channel()));
                    break;
                case EspNowItem::SlaveChannel:
                    save_slave_channel(cycle_slave_channel(load_slave_channel()));
                    break;
                case EspNowItem::SlaveRepeat:
                    save_slave_repeat_enabled(!load_slave_repeat_enabled());
                    break;
            }
            // New value applies on next AutonomousMaster / SlaveMode enter().
            // Operator returns to Menu and re-enters the mode to pick it up.
            draw();
        }
    }

    void draw_espnow() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "ESP-NOW", WHITE, BLACK, 2});

        char m_line[28];
        char s_line[28];
        char r_line[28];
        std::snprintf(m_line, sizeof(m_line), "Master: %s",
                      master_channel_label(load_master_channel()));
        std::snprintf(s_line, sizeof(s_line), "Slave:  %s",
                      slave_channel_label(load_slave_channel()));
        std::snprintf(r_line, sizeof(r_line), "Repeat: %s",
                      load_slave_repeat_enabled() ? "ON" : "OFF");
        const char* lines[kEspNowFunctionalItemCount] = { m_line, s_line, r_line };

        for (size_t i = 0; i < kEspNowFunctionalItemCount; ++i) {
            const bool sel = (i == sub_selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", lines[i]);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 80, "(applies on mode entry)",
            WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: change  B-hold: back",
            WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // PixMob submenu (PixMob-protocol-specific commands moved out of Test):
    //   Set Group ID    bracelet-setup helper, fires SetGroupId+SetGroupSel
    //   Group Target    Btn1 advances + fires to groups 1..5 in turn
    //
    // Two-level navigation within the submenu: menu lists the items, Btn1
    // drills into a workflow screen. PWR-hold pops one level (workflow ->
    // menu -> Config top).
    // -------------------------------------------------------------------------

    void handle_pixmob(const ButtonPressEvent& ev) {
        switch (pixmob_state_) {
            case PixMobState::Menu:
                if (ev.id == ButtonId::Btn2) {
                    pixmob_selected_ = (pixmob_selected_ + 1) % kPixMobItemCount;
                    draw();
                } else if (ev.id == ButtonId::Btn1) {
                    pixmob_state_ = (pixmob_selected_ == 0)
                                        ? PixMobState::SetGroupId
                                        : PixMobState::GroupTarget;
                    confirm_until_ms_ = 0;
                    draw();
                }
                break;
            case PixMobState::SetGroupId:
                if (ev.id == ButtonId::Btn2) {
                    pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                    draw();
                } else if (ev.id == ButtonId::Btn1) {
                    // Target name is irrelevant - the AssignDeviceGroup
                    // dispatch ignores device group_id and uses the event's
                    // new_group_id payload.
                    const bool ok = DAL::fire_assign_device_group(
                        "all-pixmobs",
                        AssignDeviceGroupEvent{pixmob_target_group_});
                    if (ok) confirm_until_ms_ = millis() + kConfirmFlashMs;
                    draw();
                }
                break;
            case PixMobState::GroupTarget:
                if (ev.id == ButtonId::Btn1) {
                    char target[16];
                    std::snprintf(target, sizeof(target),
                                  "group-%u", (unsigned)pixmob_target_group_);
                    DAL::fire_rgb_pulse(target, RgbPulseEvent{
                        0xFF, 0xFF, 0xFF,
                        pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS,
                        pixmob::CHANCE_100});
                    pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                    draw();
                }
                break;
        }
    }

    void draw_pixmob() {
        switch (pixmob_state_) {
            case PixMobState::Menu:        draw_pixmob_menu();      break;
            case PixMobState::SetGroupId:  draw_pixmob_set_group(); break;
            case PixMobState::GroupTarget: draw_pixmob_group_tgt(); break;
        }
    }

    void draw_pixmob_menu() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "PixMob", WHITE, BLACK, 2});

        const char* items[kPixMobItemCount] = { "Set Group ID", "Group Test" };
        for (size_t i = 0; i < kPixMobItemCount; ++i) {
            const bool sel = (i == pixmob_selected_);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", items[i]);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: enter  PWR: back",
            WHITE, BLACK, 1});
    }

    void draw_pixmob_set_group() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Set Group ID", WHITE, BLACK, 2});
        char buf[24];
        std::snprintf(buf, sizeof(buf), "New group: %u",
                      (unsigned)pixmob_target_group_);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 40, buf, WHITE, BLACK, 2});
        if (confirm_until_ms_ != 0) {
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 70, "Sent!", GREEN, BLACK, 2});
        } else {
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 70, "isolate target!", YELLOW, BLACK, 1});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 95, "B: cycle  A: send", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    void draw_pixmob_group_tgt() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Group Test", WHITE, BLACK, 2});
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Next: group %u",
                      (unsigned)pixmob_target_group_);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, buf, WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 90, "A: fire + advance", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // System submenu (functional)
    // -------------------------------------------------------------------------

    enum class SystemItem : uint8_t {
        FirmwareVersion = 0,
        DefaultBootMode,
        FactoryReset,
        BatteryStatus,
    };
    static constexpr size_t kSystemItemCount = 4;

    void handle_system(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            sub_selected_ = (sub_selected_ + 1) % kSystemItemCount;
            draw();
            return;
        }
        if (ev.id == ButtonId::Btn1) {
            switch ((SystemItem)sub_selected_) {
                case SystemItem::FactoryReset:
                    factory_reset();
                    confirm_until_ms_ = millis() + 800;   // "done" linger
                    draw();
                    break;
                default:
                    // Info-only items - no-op on Btn1.
                    break;
            }
        }
    }

    void factory_reset() {
#ifdef ARDUINO
        Preferences prefs;
        prefs.begin("noct", /*readOnly=*/false);
        prefs.clear();
        prefs.end();
#endif
        // s_last_runtime stays as in-memory state until reboot.
    }

    void draw_system() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "System", WHITE, BLACK, 2});

        char fw[28]; std::snprintf(fw, sizeof(fw), "Firmware: %s", kFirmwareVersion);
        char dm[28]; std::snprintf(dm, sizeof(dm), "Default: %s", mode_label_short(s_last_runtime));
        char br[28];
        const int batt = last_drawn_battery_ >= -1 ? last_drawn_battery_
                                                    : DAL::battery_level("local");
        if (batt < 0) std::snprintf(br, sizeof(br), "Batt: --");
        else          std::snprintf(br, sizeof(br), "Batt: %d%%", batt);

        const char* lines[kSystemItemCount] = {
            fw,
            dm,
            confirm_until_ms_ != 0 ? "Reset complete" : "Factory reset",
            br,
        };
        for (size_t i = 0; i < kSystemItemCount; ++i) {
            const bool sel = (i == sub_selected_);
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", lines[i]);
            const uint16_t fg = (i == (size_t)SystemItem::FactoryReset
                                  && confirm_until_ms_ != 0) ? GREEN
                              : sel                          ? YELLOW
                              :                                WHITE;
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 16, buf, fg, BLACK, 2});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 122, "B: cycle  A: act  PWR: back",
            WHITE, BLACK, 1});
    }

    static const char* mode_label_short(ModeId m) {
        switch (m) {
            case ModeId::AutonomousMaster: return "Master";
            case ModeId::Slave:            return "Slave";
            case ModeId::Config:           return "Config";
            case ModeId::Test:             return "Test";
            default:                       return "?";
        }
    }

    static constexpr const char* kFirmwareVersion = "1.0.0";

    void draw() {
        if (level_ == Level::Top) draw_top();
        else                       draw_sub();
    }
};

constexpr ConfigMode::TopEntry ConfigMode::kTop[8];
constexpr const char* ConfigMode::kAudioItems[];
constexpr const char* ConfigMode::kIrItems[];
constexpr const char* ConfigMode::kEspNowItems[];
constexpr const char* ConfigMode::kWifiItems[];
constexpr const char* ConfigMode::kDmxItems[];
constexpr const char* ConfigMode::kFirmwareVersion;

// =============================================================================
// TestMode - the §8.5 test-mode catalogue plus a Set Group ID helper.
//
// Two-level UI: enter Test Mode -> sub-test list. Btn2 cycles, Btn1 launches.
// Inside a sub-test, button bindings are sub-test-specific and PWR-hold
// returns to the sub-test list (not the main menu); a second PWR-hold from
// the sub-test list returns to the main menu.
//
// The seven sub-tests:
//   1. Pulse Test       - 1 Hz cycle through R/G/B/W with the punchy envelope.
//   2. Fade Test        - 1 Hz cycle through R/G/B/W with long-fade envelope.
//   3. Rainbow Test     - 6-second smooth hue cycle via the Rainbow effect.
//   4. Sparkle Test     - 10 seconds of CHANCE_50 random-palette pulses.
//   5. White Out        - one sustained white pulse per Btn1 press.
//   6. Group Targeting  - fires to groups 1-5 in turn; Btn1 advances.
//   7. Set Group ID     - bracelet-setup helper. Btn2 picks group 1-5;
//                         Btn1 transmits SetGroupId via the AssignDeviceGroup
//                         DAL capability. Belongs in Config > IR > Group ID
//                         assignment when Config lands; here for now to
//                         support hardware setup of new bracelets.
// =============================================================================

namespace {

struct PaletteColour { uint8_t r, g, b; const char* name; };

// Pulse / Fade tests want explicit colour cycling including red. Sparkle
// reads a separate palette that excludes pure red - aesthetically Sparkle
// is meant to read as cool twinkles, and the bracelet's red LED also
// runs slightly hotter than green/blue at the same drive level, so any
// red-bearing colour reads warm during the fade tail.
constexpr PaletteColour kTestPalette[] = {
    { 0xFF, 0x00, 0x00, "RED"   },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
    { 0xFF, 0xFF, 0xFF, "WHITE" },
};
constexpr size_t kTestPaletteCount = sizeof(kTestPalette) / sizeof(kTestPalette[0]);

constexpr PaletteColour kSparklePalette[] = {
    { 0xFF, 0xFF, 0xFF, "WHITE" },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
    { 0x00, 0xFF, 0xFF, "CYAN"  },
};
constexpr size_t kSparklePaletteCount =
    sizeof(kSparklePalette) / sizeof(kSparklePalette[0]);

constexpr uint32_t kPulseStepMs       = 1000;   // 1 Hz colour cycle
constexpr uint32_t kFadeStepMs        = 1000;
constexpr uint32_t kRainbowDurationMs = 6000;
constexpr uint32_t kSparkleDurationMs = 10000;
constexpr uint32_t kSparkleStepMs     = 200;

}  // namespace

class TestMode : public Mode {
public:
    ModeId id() const override { return ModeId::Test; }
    const char* name() const override { return "Test"; }

    void enter() override {
        menu_selected_    = 0;
        menu_view_offset_ = 0;
        // Same channel as AutonomousMaster (NVS-configured). Test fires
        // broadcast over ESP-NOW so any slave on the configured show
        // channel renders the colour and forwards IR to its own bracelets,
        // just like during a real show.
        broadcaster_.begin(load_master_channel());
        return_to_menu();
    }

    void exit() override {
        if (active_test_ == SubTest::AudioLive
         || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
        dal::local_driver_instance()->cancel_pulse();
        broadcaster_.end();
        active_test_ = SubTest::None;
    }

    void loop_tick() override {
        const uint32_t now = millis();
        switch (active_test_) {
            case SubTest::PulseTest:   tick_pulse_or_fade(now, /*fade=*/false); break;
            case SubTest::FadeTest:    tick_pulse_or_fade(now, /*fade=*/true);  break;
            case SubTest::RainbowTest: tick_rainbow(now);                        break;
            case SubTest::SparkleTest: tick_sparkle(now);                        break;
            case SubTest::AudioLive:   tick_audio_live(now);                     break;
            case SubTest::Calibrate:   tick_calibrate(now);                      break;
            default: break;
        }
        // Drain any pending redundant retransmits scheduled by the test's
        // initial fire (per spec §4.3 master reliability redundancy).
        broadcaster_.pump_retransmits();
        // Heartbeat the master-alive signal at 1 Hz throughout Test mode -
        // not just during a sub-test fire. Without this, sitting in the
        // test menu (or in gaps between pulse-test steps longer than the
        // slave's 3 s no-signal threshold) makes the slave declare
        // NO SIGNAL even though we're actively running. Skip-if-recent
        // means the actual bandwidth cost during music is roughly nil.
        broadcaster_.maybe_send_heartbeat();
        // Post-pulse status redraw. While a pulse is animating the screen is
        // the light surface (LocalDriver paints frame-by-frame). When the
        // pulse terminates LocalDriver paints a final BLACK frame; we then
        // overlay the test's status text on top so the operator sees the
        // sub-test name, countdown, etc. between pulses. Only fires on the
        // active->inactive transition so we don't repeatedly overdraw.
        const bool pulse_active = dal::local_driver_instance()->is_pulse_active();
        if (pulse_was_active_ && !pulse_active) {
            redraw_status_for_active_test();
        }
        pulse_was_active_ = pulse_active;
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (active_test_ == SubTest::None) handle_button_at_menu(ev);
        else                                handle_button_in_test(ev);
    }

    void on_audio_frame(const AudioFrameEvent& ev) override {
        if (active_test_ == SubTest::AudioLive) {
            process_audio_frame(ev);
        } else if (active_test_ == SubTest::Calibrate) {
            on_audio_frame_calibrate(ev);
        }
    }

private:
    enum class SubTest : uint8_t {
        None = 0,
        PulseTest,
        FadeTest,
        RainbowTest,
        SparkleTest,
        WhiteOut,
        AudioLive,
        Calibrate,
    };

    struct MenuItem { SubTest test; const char* label; };
    static constexpr MenuItem kSubTests[7] = {
        { SubTest::PulseTest,   "Pulse"        },
        { SubTest::FadeTest,    "Fade"         },
        { SubTest::RainbowTest, "Rainbow"      },
        { SubTest::SparkleTest, "Sparkle"      },
        { SubTest::WhiteOut,    "White Out"    },
        { SubTest::AudioLive,   "Audio Live"   },
        { SubTest::Calibrate,   "Calibrate"    },
    };
    static constexpr size_t kSubTestCount = sizeof(kSubTests) / sizeof(kSubTests[0]);

    SubTest  active_test_      = SubTest::None;
    size_t   menu_selected_    = 0;
    size_t   menu_view_offset_ = 0;     // top of visible window for scrolling
    uint8_t  step_index_       = 0;
    uint32_t test_start_ms_ = 0;
    uint32_t last_step_ms_  = 0;

    // Audio Live state - mirror of AutonomousMaster's beat detection plus
    // the latest band energies for the spectrum bar render. No IR fires
    // from this view; it's purely for inspecting what the mic is picking
    // up. Fields prefixed `audio_` to avoid clashing with the cycle-test
    // step_index_ / last_step_ms_ above (those get reused per launch).
    float    audio_bass_           = 0.0f;
    float    audio_mid_            = 0.0f;
    float    audio_treble_         = 0.0f;
    float    audio_rms_            = 0.0f;
    float    audio_baseline_flux_  = 100.0f;
    float    audio_prev_bass_      = 0.0f;
    float    audio_current_flux_   = 0.0f;
    uint32_t audio_last_beat_ms_   = 0;
    uint32_t audio_beat_flash_until_ = 0;
    float    audio_bpm_            = 0.0f;
    uint32_t audio_ibi_buffer_[8]  = {0};
    size_t   audio_ibi_index_      = 0;
    size_t   audio_ibi_count_      = 0;

    // Auto-cal rolling per-band min/max with leak. Used by AudioLive when
    // s_calibration.auto_enabled is true. Initialised wide (huge min, tiny
    // max) so the first frame replaces them; thereafter min creeps up at
    // ~0.5%/sec and max creeps down at ~0.5%/sec, so the bars adapt to
    // the room over a few seconds without forgetting too quickly.
    float    auto_min_[4]          = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };
    float    auto_max_[4]          = { 1.0f,   1.0f,   1.0f,   1.0f   };

    // Rainbow Test: inline hue cycling so we can fan the same RgbPulseEvent
    // out to all three targets explicitly (IR bracelets, local screen,
    // ESP-NOW broadcast). The effects::Rainbow class only fires to a
    // single target; using it here meant slaves never received the rainbow
    // colour over the wire. When the EspNowDriver lands and "esp-now-
    // broadcast" becomes a registered DAL target, this can collapse back
    // into a single Rainbow instance whose target is "esp-now-broadcast"
    // and the driver fans out internally.
    static constexpr float    kRainbowCycleHz       = 0.5f;
    static constexpr float    kRainbowBrightness    = 1.0f;
    static constexpr uint16_t kRainbowStepIntervalMs = 50;
    float    rainbow_hue_         = 0.0f;
    uint32_t rainbow_last_step_ms_ = 0;

    // Test mode broadcasts on the same channel AutonomousMaster uses
    // (NVS-configured: 1 hobby / 11 show / 6 custom).
    EspNowBroadcaster broadcaster_;

    // Tracks the LocalDriver's pulse-active flag across loop_tick calls,
    // so we can redraw status text on the falling edge.
    bool pulse_was_active_ = false;

    void redraw_status_for_active_test() {
        switch (active_test_) {
            case SubTest::PulseTest:    draw_cycle_screen("Pulse");  break;
            case SubTest::FadeTest:     draw_cycle_screen("Fade");   break;
            case SubTest::RainbowTest:  draw_rainbow_screen();       break;
            case SubTest::SparkleTest:  draw_sparkle_screen();       break;
            case SubTest::WhiteOut:     draw_whiteout();             break;
            // AudioLive and Calibrate own their own draw cadence; no
            // post-pulse redraw needed (they don't drive RgbPulse).
            default: break;
        }
    }

    // -------------------------------------------------------------------------
    // Sub-test menu
    // -------------------------------------------------------------------------

    void return_to_menu() {
        if (active_test_ == SubTest::AudioLive
         || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
        // Stop any in-flight pulse animation before redrawing the menu;
        // otherwise the next loop_tick frame would overdraw the menu list.
        dal::local_driver_instance()->cancel_pulse();
        pulse_was_active_ = false;
        active_test_ = SubTest::None;
        draw_menu();
    }

    static constexpr size_t kMenuVisible = 8;

    void draw_menu() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        // 8 items at size 2 + 16 px line spacing = 128 px content + 4 px
        // top margin = 132 px, just inside the 135 px display. When the
        // sub-test list grows past 8, the cursor scrolls the visible
        // window via menu_view_offset_. Title and bottom hint dropped to
        // make room; PWR-hold returns to the main mode menu, and B/A
        // conventions match the other menus.
        const size_t visible = (kSubTestCount < kMenuVisible)
                                   ? kSubTestCount : kMenuVisible;
        for (size_t row = 0; row < visible; ++row) {
            const size_t i = row + menu_view_offset_;
            if (i >= kSubTestCount) break;
            const bool sel = (i == menu_selected_);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", kSubTests[i].label);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 4 + (int)row * 16, buf,
                sel ? YELLOW : WHITE, BLACK, 2});
        }
    }

    void update_menu_view_offset() {
        if (menu_selected_ < menu_view_offset_) {
            menu_view_offset_ = menu_selected_;
        } else if (menu_selected_ >= menu_view_offset_ + kMenuVisible) {
            menu_view_offset_ = menu_selected_ - kMenuVisible + 1;
        }
    }

    void handle_button_at_menu(const ButtonPressEvent& ev) {
        if (ev.kind != ButtonEvent::Pressed
         && ev.kind != ButtonEvent::LongPressed) return;
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
            menu_selected_ = (menu_selected_ + 1) % kSubTestCount;
            // Wrapping past the bottom resets the scroll offset; otherwise
            // shift the visible window so the cursor stays on screen.
            if (menu_selected_ == 0) menu_view_offset_ = 0;
            else                     update_menu_view_offset();
            draw_menu();
            return;
        }
        if (ev.id == ButtonId::Btn1 && ev.kind == ButtonEvent::Pressed) {
            launch_test(kSubTests[menu_selected_].test);
            return;
        }
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
            ModeMachine::switch_to(ModeId::Menu);
            return;
        }
    }

    // -------------------------------------------------------------------------
    // Sub-test launch + per-sub-test button handling
    // -------------------------------------------------------------------------

    void launch_test(SubTest t) {
        active_test_     = t;
        step_index_      = 0;
        test_start_ms_   = millis();
        last_step_ms_    = 0;
        switch (t) {
            case SubTest::PulseTest:    enter_pulse_or_fade(/*fade=*/false); break;
            case SubTest::FadeTest:     enter_pulse_or_fade(/*fade=*/true);  break;
            case SubTest::RainbowTest:  enter_rainbow();                      break;
            case SubTest::SparkleTest:  enter_sparkle();                      break;
            case SubTest::WhiteOut:     draw_whiteout();                      break;
            case SubTest::AudioLive:    enter_audio_live();                   break;
            case SubTest::Calibrate:    enter_calibrate();                    break;
            default: break;
        }
    }

    void handle_button_in_test(const ButtonPressEvent& ev) {
        if (ev.kind != ButtonEvent::Pressed
         && ev.kind != ButtonEvent::LongPressed) return;

        // BtnB long-press always goes back one level (sub-test -> sub-test
        // menu).
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
            return_to_menu();
            return;
        }

        if (ev.kind != ButtonEvent::Pressed) return;
        switch (active_test_) {
            case SubTest::WhiteOut:    if (ev.id == ButtonId::Btn1) fire_whiteout(); break;
            case SubTest::Calibrate:
                handle_button_calibrate(ev);
                break;
            default:
                // Continuous tests (Pulse/Fade/Rainbow/Sparkle/AudioLive)
                // ignore button presses other than the back gesture.
                break;
        }
    }

    // -------------------------------------------------------------------------
    // Pulse / Fade Test (1 Hz cycle through palette)
    // -------------------------------------------------------------------------

    void enter_pulse_or_fade(bool fade) {
        step_index_   = 0;
        last_step_ms_ = 0;
        draw_cycle_screen(fade ? "Fade" : "Pulse");
        fire_cycle_step(fade);
    }

    void tick_pulse_or_fade(uint32_t now, bool fade) {
        if (now - last_step_ms_ < (fade ? kFadeStepMs : kPulseStepMs)) return;
        step_index_   = (step_index_ + 1) % kTestPaletteCount;
        fire_cycle_step(fade);
        // Status text redraws via the post-pulse hook in loop_tick - if we
        // redraw here we'd overdraw the pulse animation immediately.
    }

    void fire_cycle_step(bool fade) {
        const auto& c = kTestPalette[step_index_];
        const pixmob::Time attack  = fade ? pixmob::T_192_MS : pixmob::T_32_MS;
        const pixmob::Time sustain = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
        const pixmob::Time release = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
        const RgbPulseEvent ev{
            c.r, c.g, c.b, attack, sustain, release, pixmob::CHANCE_100};
        DAL::render_fx("all-pixmobs", ev);
        DAL::render_fx("local",       ev);  // screen; gated by Config > Display > Pulse
        broadcaster_.send_light_command(/*target_group=*/0, ev);
        last_step_ms_ = millis();
    }

    void draw_cycle_screen(const char* title) {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, title, WHITE, BLACK, 3});
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Colour: %s",
                      kTestPalette[step_index_].name);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, buf, WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 100, "1 Hz auto", WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Rainbow Test (6 sec via Rainbow effect)
    // -------------------------------------------------------------------------

    void enter_rainbow() {
        rainbow_hue_         = 0.0f;
        rainbow_last_step_ms_ = 0;
        draw_rainbow_screen();
    }

    void tick_rainbow(uint32_t now) {
        if (now - test_start_ms_ >= kRainbowDurationMs) {
            return_to_menu();
            return;
        }
        if (now - rainbow_last_step_ms_ < kRainbowStepIntervalMs) return;
        rainbow_last_step_ms_ = now;

        // Advance hue: degrees-per-step = 360 * cycle_hz / steps_per_sec.
        const float steps_per_sec = 1000.0f / (float)kRainbowStepIntervalMs;
        const float deg_per_step  = 360.0f * kRainbowCycleHz / steps_per_sec;
        rainbow_hue_ = std::fmod(rainbow_hue_ + deg_per_step, 360.0f);

        uint8_t r, g, b;
        effects::hsv_to_rgb(rainbow_hue_, 1.0f, kRainbowBrightness, r, g, b);

        // Envelope: attack=0, sustain=96, release=0 - matches the original
        // effects::Rainbow tuning. Each step's command lands during the
        // previous step's sustain so the bracelet stays at full brightness
        // for the whole hue cycle (no fade-to-dark gaps).
        const RgbPulseEvent ev{
            r, g, b,
            pixmob::T_0_MS, pixmob::T_96_MS, pixmob::T_0_MS,
            pixmob::CHANCE_100};
        DAL::render_fx("all-pixmobs", ev);
        DAL::render_fx("local",       ev);
        broadcaster_.send_light_command(/*target_group=*/0, ev);
    }

    void draw_rainbow_screen() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Rainbow", WHITE, BLACK, 3});
        const uint32_t elapsed   = millis() - test_start_ms_;
        const uint32_t remaining = (elapsed < kRainbowDurationMs)
                                       ? (kRainbowDurationMs - elapsed) : 0;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lu s left", (unsigned long)(remaining / 1000 + 1));
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 60, buf, WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Sparkle Test (10 sec of CHANCE_50 random-palette pulses)
    // -------------------------------------------------------------------------

    void enter_sparkle() {
        last_step_ms_ = 0;
        draw_sparkle_screen();
    }

    void tick_sparkle(uint32_t now) {
        if (now - test_start_ms_ >= kSparkleDurationMs) {
            return_to_menu();
            return;
        }
        if (now - last_step_ms_ < kSparkleStepMs) return;
        const auto& c = kSparklePalette[std::rand() % kSparklePaletteCount];
        const RgbPulseEvent ev{
            c.r, c.g, c.b,
            pixmob::T_32_MS, pixmob::T_32_MS, pixmob::T_96_MS,
            pixmob::CHANCE_50};
        DAL::render_fx("all-pixmobs", ev);
        DAL::render_fx("local",       ev);   // LocalDriver rolls CHANCE_50
                                             // independently, like a bracelet
        broadcaster_.send_light_command(/*target_group=*/0, ev);
        last_step_ms_ = now;
        // Status text redraws via the post-pulse hook in loop_tick.
    }

    void draw_sparkle_screen() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Sparkle", WHITE, BLACK, 3});
        const uint32_t elapsed   = millis() - test_start_ms_;
        const uint32_t remaining = (elapsed < kSparkleDurationMs)
                                       ? (kSparkleDurationMs - elapsed) : 0;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%lu s left", (unsigned long)(remaining / 1000 + 1));
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 60, buf, WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // White Out (sustained white on Btn1, repeatable)
    // -------------------------------------------------------------------------

    void fire_whiteout() {
        // Single command: instant attack, ~2.4 s sustain, ~0.96 s release.
        // The PixMob protocol's Time enum has values up to T_3840_MS so
        // we don't need a multi-command staircase to span 2 s + 1 s.
        const RgbPulseEvent ev{
            0xFF, 0xFF, 0xFF,
            pixmob::T_0_MS, pixmob::T_2400_MS, pixmob::T_960_MS,
            pixmob::CHANCE_100};
        DAL::render_fx("all-pixmobs", ev);
        DAL::render_fx("local",       ev);
        broadcaster_.send_light_command(/*target_group=*/0, ev);
    }

    void draw_whiteout() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "White Out", WHITE, BLACK, 3});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 60, "A: fire white", WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Audio Live (4 spectrum bars + flux/threshold + BPM + beat indicator)
    // -------------------------------------------------------------------------

    void enter_audio_live() {
        audio_baseline_flux_     = 100.0f;
        audio_prev_bass_         = 0.0f;
        audio_current_flux_      = 0.0f;
        audio_last_beat_ms_      = 0;
        audio_beat_flash_until_  = 0;
        audio_bpm_               = 0.0f;
        audio_ibi_index_         = 0;
        audio_ibi_count_         = 0;
        audio_bass_ = audio_mid_ = audio_treble_ = audio_rms_ = 0.0f;
        for (int i = 0; i < 4; ++i) {
            auto_min_[i] = 1.0e9f;
            auto_max_[i] = 1.0f;
        }
        last_step_ms_            = 0;
        DAL::start_audio_input("local", 16000, 512);
        draw_audio_live_static();
        draw_audio_live_dynamic();
    }

    void process_audio_frame(const AudioFrameEvent& ev) {
        audio_bass_   = ev.bass_energy;
        audio_mid_    = ev.mid_energy;
        audio_treble_ = ev.treble_energy;
        audio_rms_    = ev.overall_rms;

        // Auto-cal: adapt per-band min/max with a slow leak so the bars
        // track room dynamics. Frame rate ~30 Hz; 1.0001/0.9999 per frame
        // leaks ~0.3%/sec.
        if (s_calibration.auto_enabled) {
            const float bands[4] = {
                ev.bass_energy, ev.mid_energy, ev.treble_energy, ev.overall_rms
            };
            for (int i = 0; i < 4; ++i) {
                if (bands[i] > 0.0f && bands[i] < auto_min_[i]) auto_min_[i] = bands[i];
                else                                            auto_min_[i] *= 1.0001f;
                if (bands[i] > auto_max_[i])                    auto_max_[i] = bands[i];
                else                                            auto_max_[i] *= 0.9999f;
                if (auto_min_[i] < 1.0f) auto_min_[i] = 1.0f;
                // Floor the rolling dynamic range at 5 octaves (32x). The
                // original 2-octave constraint worked on the Plus2's PDM mic
                // because its noise floor was variable enough that auto_min
                // and auto_max stayed naturally apart, but the StickS3's
                // ES8311 codec has a much flatter noise floor and the bars
                // would compress to a tiny range and then oscillate wildly
                // with any input variance. 5 octaves comfortably accommodates
                // music dynamics on either host.
                if (auto_max_[i] < auto_min_[i] * 32.0f) {
                    auto_max_[i] = auto_min_[i] * 32.0f;
                }
            }
        }

        // Beat firing now driven by the DAL analyser's BeatDetector
        // (Epic 4.5 Block 3), so what you see here matches exactly what
        // would fire in Master mode. The flux/baseline tracking below
        // remains for the diagnostic meter strip - it shows what the
        // legacy single-threshold detector would have seen at this
        // frame, decoupled from the actual beat decision.
        constexpr float kVolumeGate     = 500.0f;
        constexpr float kBaselineAlpha  = 0.02f;

        if (ev.overall_rms < kVolumeGate) {
            audio_prev_bass_ = 0.0f;
            return;
        }
        float flux = ev.bass_energy - audio_prev_bass_;
        if (flux < 0) flux = 0;
        audio_prev_bass_    = ev.bass_energy;
        audio_current_flux_ = flux;
        audio_baseline_flux_ = audio_baseline_flux_ * (1.0f - kBaselineAlpha)
                             + flux * kBaselineAlpha;

        const uint32_t now = millis();
        if (!ev.is_beat) return;

        if (audio_last_beat_ms_ > 0) {
            const uint32_t ibi = now - audio_last_beat_ms_;
            if (ibi >= 300 && ibi <= 1200) {
                audio_ibi_buffer_[audio_ibi_index_] = ibi;
                audio_ibi_index_ = (audio_ibi_index_ + 1) % 8;
                if (audio_ibi_count_ < 8) audio_ibi_count_++;
                update_audio_bpm();
            }
        }
        audio_last_beat_ms_     = now;
        audio_beat_flash_until_ = now + 120;
    }

    void update_audio_bpm() {
        if (audio_ibi_count_ < 3) return;
        uint32_t sorted[8];
        for (size_t i = 0; i < audio_ibi_count_; ++i) sorted[i] = audio_ibi_buffer_[i];
        for (size_t i = 1; i < audio_ibi_count_; ++i) {
            uint32_t key = sorted[i];
            size_t j = i;
            while (j > 0 && sorted[j-1] > key) { sorted[j] = sorted[j-1]; --j; }
            sorted[j] = key;
        }
        const uint32_t med = (audio_ibi_count_ % 2 == 1)
            ? sorted[audio_ibi_count_ / 2]
            : (sorted[audio_ibi_count_ / 2 - 1] + sorted[audio_ibi_count_ / 2]) / 2;
        if (med > 50) audio_bpm_ = 60000.0f / (float)med;
    }

    void tick_audio_live(uint32_t now) {
        // Refresh dynamic elements at ~20 Hz to keep bars smooth without
        // saturating IR-bus or display refresh.
        if (now - last_step_ms_ < 50) return;
        last_step_ms_ = now;
        draw_audio_live_dynamic();
    }

    void draw_audio_live_static() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 2, "Audio Live", WHITE, BLACK, 1});
        // Bar baselines (top + bottom outline lines). Per-band labels are
        // drawn alongside the dynamic numeric readout so they live in
        // draw_audio_live_dynamic, not here.
        const int bar_y_top    = 14;
        const int bar_y_bottom = 84;
        const int bar_w        = 42;
        for (int i = 0; i < 4; ++i) {
            const int x = 10 + i * 56;
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                x, bar_y_top,    bar_w, 1, WHITE});
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                x, bar_y_bottom, bar_w, 1, WHITE});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B-hold: back", WHITE, BLACK, 1});
    }

    void draw_audio_live_dynamic() {
        // Bar fills. Log2 scaling - bass/mid/treble band sums span several
        // decades (mid sums 57 FFT bins, treble 191). Per-band floor/
        // ceiling come from s_calibration (set via Calibrate sub-test) or
        // from the rolling auto-cal min/max when auto mode is on (live
        // audience use, where the room's noise floor moves mid-show).
        const int   bar_y_top    = 14;
        const int   bar_y_bottom = 84;
        const int   bar_w        = 42;
        const int   bar_h_max    = bar_y_bottom - bar_y_top - 2;
        float floors[4];
        float ceils[4];
        if (s_calibration.auto_enabled) {
            for (int i = 0; i < 4; ++i) {
                floors[i] = std::log2f(auto_min_[i]);
                ceils[i]  = std::log2f(auto_max_[i]);
                if (ceils[i] - floors[i] < 2.0f) ceils[i] = floors[i] + 2.0f;
            }
        } else {
            for (int i = 0; i < 4; ++i) {
                floors[i] = s_calibration.floor[i];
                ceils[i]  = s_calibration.ceil[i];
            }
        }
        const char  band_labels[4] = { 'B', 'M', 'T', 'R' };
        const float values[4]    = { audio_bass_, audio_mid_, audio_treble_, audio_rms_ };
        for (int i = 0; i < 4; ++i) {
            const int x = 10 + i * 56;
            // Clear the inner area (between the top/bottom outline lines).
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                x, bar_y_top + 1, bar_w, bar_h_max, BLACK});
            float v = values[i];
            if (v < 1.0f) v = 1.0f;
            const float lg    = std::log2f(v);
            float ratio = (lg - floors[i]) / (ceils[i] - floors[i]);
            if (ratio < 0)   ratio = 0;
            if (ratio > 1.f) ratio = 1.f;
            const int h = (int)(ratio * (float)bar_h_max);
            if (h > 0) {
                DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                    x, bar_y_bottom - h, bar_w, h, GREEN});
            }
            // Diagnostic readout below each bar: "B 12k" / "M 568k" / etc.
            // Right-padded with spaces in snprintf to overwrite previous
            // longer strings cleanly.
            char vb[10];
            if (values[i] >= 1000000.0f) {
                std::snprintf(vb, sizeof(vb), "%c %.0fM   ",
                              band_labels[i], (double)(values[i] / 1.0e6f));
            } else if (values[i] >= 10000.0f) {
                std::snprintf(vb, sizeof(vb), "%c %.0fk   ",
                              band_labels[i], (double)(values[i] / 1.0e3f));
            } else {
                std::snprintf(vb, sizeof(vb), "%c %4.0f  ",
                              band_labels[i], (double)values[i]);
            }
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                x, bar_y_bottom + 4, vb, WHITE, BLACK, 1});
        }

        // Flux + threshold + BPM lines (overwrite with bg=BLACK).
        char info[40];
        std::snprintf(info, sizeof(info), "Flux:%5.0f Thr:%5.0f",
                      (double)audio_current_flux_,
                      (double)(audio_baseline_flux_ * 2.5f));
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 100, info, WHITE, BLACK, 1});

        char bpm_buf[24];
        if (audio_bpm_ > 0.0f) {
            std::snprintf(bpm_buf, sizeof(bpm_buf), "BPM: %3.0f",
                          (double)audio_bpm_);
        } else {
            std::snprintf(bpm_buf, sizeof(bpm_buf), "BPM: ---");
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 114, bpm_buf, WHITE, BLACK, 1});

        // Beat indicator: lit briefly after each detected beat.
        const bool active = audio_beat_flash_until_ > millis();
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            120, 114, active ? "BEAT!" : "     ",
            active ? YELLOW : BLACK, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Calibrate (sound-check workflow + auto-cal toggle, persists to NVS)
    // -------------------------------------------------------------------------
    //
    // Manual sound check is two phases: 3 s of silence (captures rolling
    // min into floors), then 10 s of music (captures rolling max into
    // ceilings). Both pass through the AudioLive process_audio_frame path
    // so the bracelet still hears nothing - this view doesn't fire IR.
    //
    // Auto mode is for live-audience use, where the room's noise floor
    // shifts mid-show as the crowd builds and the sound system warms up;
    // the AudioLive bars fall back to rolling per-band min/max instead
    // of the saved floors/ceilings so they stay readable.

    enum class CalState : uint8_t {
        Menu = 0,
        BaselinePrompt,
        BaselineCapture,
        PeakPrompt,
        PeakCapture,
        Done,
    };

    static constexpr uint32_t kCalBaselineMs = 3000;
    static constexpr uint32_t kCalPeakMs     = 10000;
    static constexpr uint32_t kCalDoneMs     = 1500;

    CalState  cal_state_           = CalState::Menu;
    uint32_t  cal_phase_start_ms_  = 0;
    uint32_t  cal_last_redraw_ms_  = 0;
    float     cal_min_[4]          = {0.0f, 0.0f, 0.0f, 0.0f};
    float     cal_max_[4]          = {0.0f, 0.0f, 0.0f, 0.0f};

    void enter_calibrate() {
        cal_state_           = CalState::Menu;
        cal_phase_start_ms_  = 0;
        cal_last_redraw_ms_  = 0;
        DAL::start_audio_input("local", 16000, 512);
        draw_calibrate();
    }

    void on_audio_frame_calibrate(const AudioFrameEvent& ev) {
        const float bands[4] = {
            ev.bass_energy, ev.mid_energy, ev.treble_energy, ev.overall_rms
        };
        if (cal_state_ == CalState::BaselineCapture) {
            for (int i = 0; i < 4; ++i) {
                if (bands[i] > 0.0f
                 && (cal_min_[i] == 0.0f || bands[i] < cal_min_[i])) {
                    cal_min_[i] = bands[i];
                }
            }
        } else if (cal_state_ == CalState::PeakCapture) {
            for (int i = 0; i < 4; ++i) {
                if (bands[i] > cal_max_[i]) cal_max_[i] = bands[i];
            }
        }
    }

    void tick_calibrate(uint32_t now) {
        switch (cal_state_) {
            case CalState::BaselineCapture:
                if (now - cal_phase_start_ms_ >= kCalBaselineMs) {
                    // Convert min energies to log2 floors with a small
                    // headroom so quiet readings settle just above 0%.
                    for (int i = 0; i < 4; ++i) {
                        if (cal_min_[i] > 0.0f) {
                            s_calibration.floor[i] = std::log2f(cal_min_[i]) + 0.3f;
                        }
                    }
                    cal_state_ = CalState::PeakPrompt;
                    draw_calibrate();
                } else if (now - cal_last_redraw_ms_ > 250) {
                    draw_calibrate();
                }
                break;
            case CalState::PeakCapture:
                if (now - cal_phase_start_ms_ >= kCalPeakMs) {
                    for (int i = 0; i < 4; ++i) {
                        if (cal_max_[i] > 0.0f) {
                            s_calibration.ceil[i] = std::log2f(cal_max_[i]);
                        }
                    }
                    save_calibration(s_calibration);
                    cal_state_          = CalState::Done;
                    cal_phase_start_ms_ = now;
                    draw_calibrate();
                } else if (now - cal_last_redraw_ms_ > 250) {
                    draw_calibrate();
                }
                break;
            case CalState::Done:
                if (now - cal_phase_start_ms_ >= kCalDoneMs) {
                    cal_state_ = CalState::Menu;
                    draw_calibrate();
                }
                break;
            default:
                break;
        }
    }

    void handle_button_calibrate(const ButtonPressEvent& ev) {
        if (ev.kind != ButtonEvent::Pressed) return;
        if (ev.id == ButtonId::Btn1) {
            switch (cal_state_) {
                case CalState::Menu:
                    cal_state_ = CalState::BaselinePrompt;
                    draw_calibrate();
                    break;
                case CalState::BaselinePrompt:
                    cal_state_          = CalState::BaselineCapture;
                    cal_phase_start_ms_ = millis();
                    for (int i = 0; i < 4; ++i) cal_min_[i] = 0.0f;
                    draw_calibrate();
                    break;
                case CalState::PeakPrompt:
                    cal_state_          = CalState::PeakCapture;
                    cal_phase_start_ms_ = millis();
                    for (int i = 0; i < 4; ++i) cal_max_[i] = 0.0f;
                    draw_calibrate();
                    break;
                default:
                    break;
            }
            return;
        }
        if (ev.id == ButtonId::Btn2 && cal_state_ == CalState::Menu) {
            s_calibration.auto_enabled = !s_calibration.auto_enabled;
            save_calibration(s_calibration);
            draw_calibrate();
        }
    }

    void draw_calibrate() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Calibrate", WHITE, BLACK, 2});
        cal_last_redraw_ms_ = millis();

        switch (cal_state_) {
            case CalState::Menu: {
                char auto_buf[24];
                std::snprintf(auto_buf, sizeof(auto_buf), "Auto: %s",
                              s_calibration.auto_enabled ? "ON" : "OFF");
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 28, auto_buf,
                    s_calibration.auto_enabled ? GREEN : WHITE, BLACK, 2});

                char fl[40], cl[40];
                std::snprintf(fl, sizeof(fl), "F: %2.0f %2.0f %2.0f %2.0f",
                              (double)s_calibration.floor[0],
                              (double)s_calibration.floor[1],
                              (double)s_calibration.floor[2],
                              (double)s_calibration.floor[3]);
                std::snprintf(cl, sizeof(cl), "C: %2.0f %2.0f %2.0f %2.0f",
                              (double)s_calibration.ceil[0],
                              (double)s_calibration.ceil[1],
                              (double)s_calibration.ceil[2],
                              (double)s_calibration.ceil[3]);
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 56, fl, WHITE, BLACK, 1});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 70, cl, WHITE, BLACK, 1});

                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 92, "A: sound check", WHITE, BLACK, 1});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 105, "B: toggle auto", WHITE, BLACK, 1});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 128, "B-hold: back", WHITE, BLACK, 1});
                break;
            }
            case CalState::BaselinePrompt:
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 36, "Stay quiet", WHITE, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 64, "for 3 seconds", WHITE, BLACK, 1});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 88, "A: start", YELLOW, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 128, "B-hold: back", WHITE, BLACK, 1});
                break;
            case CalState::BaselineCapture: {
                const uint32_t elapsed   = millis() - cal_phase_start_ms_;
                const uint32_t remaining = (kCalBaselineMs > elapsed)
                                               ? (kCalBaselineMs - elapsed) : 0;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "Recording.. %lu",
                              (unsigned long)(remaining / 1000 + 1));
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 50, buf, GREEN, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 88, "(silence)", WHITE, BLACK, 1});
                break;
            }
            case CalState::PeakPrompt:
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 36, "Play music!", WHITE, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 64, "loud + varied", WHITE, BLACK, 1});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 88, "A: start", YELLOW, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 128, "B-hold: back", WHITE, BLACK, 1});
                break;
            case CalState::PeakCapture: {
                const uint32_t elapsed   = millis() - cal_phase_start_ms_;
                const uint32_t remaining = (kCalPeakMs > elapsed)
                                               ? (kCalPeakMs - elapsed) : 0;
                char buf[24];
                std::snprintf(buf, sizeof(buf), "Recording.. %2lu",
                              (unsigned long)(remaining / 1000 + 1));
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 50, buf, GREEN, BLACK, 2});
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 88, "(music)", WHITE, BLACK, 1});
                break;
            }
            case CalState::Done:
                DAL::fire_display_show_text("local", DisplayShowTextEvent{
                    10, 50, "Saved!", GREEN, BLACK, 3});
                break;
        }
    }
};

constexpr TestMode::MenuItem TestMode::kSubTests[7];

// =============================================================================
// Per-mode singletons + lookup
// =============================================================================

namespace {

BootMode             s_boot;
MenuMode             s_menu;
AutonomousMasterMode s_autonomous_master;
SlaveMode            s_slave;
ConfigMode           s_config;
TestMode             s_test;

Mode* mode_instance(ModeId id) {
    switch (id) {
        case ModeId::Boot:             return &s_boot;
        case ModeId::Menu:             return &s_menu;
        case ModeId::AutonomousMaster: return &s_autonomous_master;
        case ModeId::Slave:            return &s_slave;
        case ModeId::Config:           return &s_config;
        case ModeId::Test:             return &s_test;
    }
    return nullptr;
}

}  // namespace

// =============================================================================
// DAL event routing
// =============================================================================

namespace {

void on_dal_button_press(const char*, const ButtonPressEvent& ev) {
    if (s_active_mode) s_active_mode->on_button_event(ev);
}

void on_dal_audio_frame(const char*, const AudioFrameEvent& ev) {
    if (s_active_mode) s_active_mode->on_audio_frame(ev);
}

}  // namespace

// =============================================================================
// ModeMachine public API
// =============================================================================

void ModeMachine::begin() {
    DAL::subscribe_button_presses("local", &on_dal_button_press);
    DAL::subscribe_audio_frames  ("local", &on_dal_audio_frame);

    s_last_runtime = load_last_runtime_mode();
    DAL::set_driver_enabled("ir-pixmob", load_ir_enabled());
    dal::local_driver_instance()->set_pulse_enabled(load_screen_pulse_enabled());
    s_calibration  = load_calibration();
    s_active_mode  = nullptr;          // force enter() in enter_mode()
    enter_mode(ModeId::Boot);
}

void ModeMachine::loop_tick() {
    if (s_active_mode) s_active_mode->loop_tick();
}

void ModeMachine::switch_to(ModeId target) {
    enter_mode(target);
}

ModeId ModeMachine::current() {
    return s_active_mode ? s_active_mode->id() : ModeId::Boot;
}

const char* ModeMachine::current_name() {
    return s_active_mode ? s_active_mode->name() : "Boot";
}

}  // namespace modes
}  // namespace nocturnation
