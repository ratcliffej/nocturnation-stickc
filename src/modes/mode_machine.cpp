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

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
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
bool             load_ir_enabled()        { return true; }
void             save_ir_enabled(bool)    {}
AudioCalibration load_calibration()       { return kCalibrationDefault; }
void             save_calibration(const AudioCalibration&) {}
#endif

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
        draw();
    }

    void exit() override {
        DAL::stop_audio_input("local");
        pulse_.exit();
    }

    void loop_tick() override {
        const uint32_t now = millis();
        if (now - last_draw_ms_ > 50) {
            draw();
            last_draw_ms_ = now;
        }
    }

    void on_audio_frame(const AudioFrameEvent& ev) override {
        current_level_ = ev.overall_rms;

        if (current_level_ < kVolumeGate) {
            prev_bass_energy_ = 0.0f;
            return;
        }

        float flux = ev.bass_energy - prev_bass_energy_;
        if (flux < 0) flux = 0;
        prev_bass_energy_ = ev.bass_energy;
        current_flux_     = flux;

        baseline_flux_ = baseline_flux_ * (1.0f - kBaselineAlpha)
                       + flux * kBaselineAlpha;

        const uint32_t now = millis();
        const bool is_beat = flux > baseline_flux_ * kBeatMultiplier
                          && flux > kFluxFloor
                          && (now - last_beat_ms_) > kBeatRefractoryMs;
        if (!is_beat) return;

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
        // Btn3 (PWR) long-press: back to menu.
        if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::LongPressed) {
            ModeMachine::switch_to(ModeId::Menu);
            return;
        }
    }

private:
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
// SlaveMode - waiting screen stub. Real ESP-NOW listening lands in Epic 4.
// =============================================================================

class SlaveMode : public Mode {
public:
    ModeId id() const override { return ModeId::Slave; }
    const char* name() const override { return "Slave"; }

    void enter() override {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 10, "Slave", WHITE, BLACK, 3});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, "Listening for master...", WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 80, "(ESP-NOW: Epic 4)", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "hold PWR for menu", WHITE, BLACK, 1});
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::LongPressed) {
            ModeMachine::switch_to(ModeId::Menu);
        }
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

        // PWR-hold pops one level. PixMob's two-level structure (menu ->
        // SetGroupId/GroupTarget workflow) gets an extra pop step before
        // it returns to the Config top-level.
        if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::LongPressed) {
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
        None = 0, Audio, IR, EspNow, WiFi, Dmx, PixMob, System,
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
    static constexpr TopEntry kTop[7] = {
        { SubMenu::Audio,  "Audio"   },
        { SubMenu::IR,     "IR"      },
        { SubMenu::EspNow, "ESP-NOW" },
        { SubMenu::WiFi,   "WiFi"    },
        { SubMenu::Dmx,    "DMX"     },
        { SubMenu::PixMob, "PixMob"  },
        { SubMenu::System, "System"  },
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
            case SubMenu::System: handle_system(ev); break;
            case SubMenu::IR:     handle_ir(ev);     break;
            case SubMenu::PixMob: handle_pixmob(ev); break;
            case SubMenu::Audio:
            case SubMenu::EspNow:
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
            case SubMenu::Audio:  draw_stub("Audio",   kAudioItems,  kAudioItemCount,  "Epic 3"); break;
            case SubMenu::IR:     draw_ir(); break;
            case SubMenu::EspNow: draw_stub("ESP-NOW", kEspNowItems, kEspNowItemCount, "Epic 4"); break;
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
            case SubMenu::Audio:  return kAudioItemCount;
            case SubMenu::IR:     return kIrItemCount;
            case SubMenu::EspNow: return kEspNowItemCount;
            case SubMenu::WiFi:   return kWifiItemCount;
            case SubMenu::Dmx:    return kDmxItemCount;
            default:              return 1;
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
    // IR submenu (functional: Enable toggles + persists; Protocol/GroupID info)
    // -------------------------------------------------------------------------

    enum class IRItem : uint8_t {
        EnableDisable = 0,
        Protocol,
        GroupIdAssignment,
    };
    static constexpr size_t kIrFunctionalItemCount = 3;

    void handle_ir(const ButtonPressEvent& ev) {
        if (ev.id == ButtonId::Btn2) {
            sub_selected_ = (sub_selected_ + 1) % kIrFunctionalItemCount;
            draw();
            return;
        }
        if (ev.id == ButtonId::Btn1
         && (IRItem)sub_selected_ == IRItem::EnableDisable) {
            const bool next = !DAL::driver_enabled("ir-pixmob");
            DAL::set_driver_enabled("ir-pixmob", next);
            save_ir_enabled(next);
            draw();
        }
        // Protocol and GroupIdAssignment are info-only - no Btn1 action.
    }

    void draw_ir() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "IR", WHITE, BLACK, 2});

        char ena[24];
        std::snprintf(ena, sizeof(ena), "Enable: %s",
                      DAL::driver_enabled("ir-pixmob") ? "ON" : "OFF");
        const char* lines[kIrFunctionalItemCount] = {
            ena,
            "Protocol: PixMob",
            "Group: Test menu",
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
            10, 122, "B: cycle  A: act  PWR: back",
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
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
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
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
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

constexpr ConfigMode::TopEntry ConfigMode::kTop[7];
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
        return_to_menu();
    }

    void exit() override {
        if (active_test_ == SubTest::RainbowTest) rainbow_.exit();
        if (active_test_ == SubTest::AudioLive
         || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
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

    effects::Rainbow rainbow_{"all-pixmobs", 0.5f, 1.0f};

    // -------------------------------------------------------------------------
    // Sub-test menu
    // -------------------------------------------------------------------------

    void return_to_menu() {
        if (active_test_ == SubTest::RainbowTest) rainbow_.exit();
        if (active_test_ == SubTest::AudioLive
         || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
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
        if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::LongPressed) {
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

        // PWR-hold always goes back one level (sub-test -> sub-test menu).
        if (ev.id == ButtonId::Btn3 && ev.kind == ButtonEvent::LongPressed) {
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
        draw_cycle_screen(fade ? "Fade" : "Pulse");
    }

    void fire_cycle_step(bool fade) {
        const auto& c = kTestPalette[step_index_];
        const pixmob::Time attack  = fade ? pixmob::T_192_MS : pixmob::T_32_MS;
        const pixmob::Time sustain = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
        const pixmob::Time release = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
        DAL::fire_rgb_pulse("all-pixmobs", RgbPulseEvent{
            c.r, c.g, c.b, attack, sustain, release, pixmob::CHANCE_100});
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
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Rainbow Test (6 sec via Rainbow effect)
    // -------------------------------------------------------------------------

    void enter_rainbow() {
        rainbow_.enter();
        draw_rainbow_screen();
    }

    void tick_rainbow(uint32_t now) {
        if (now - test_start_ms_ >= kRainbowDurationMs) {
            return_to_menu();
            return;
        }
        rainbow_.loop_tick(now);
        // Redraw countdown roughly twice per second.
        if (now - last_step_ms_ > 500) {
            draw_rainbow_screen();
            last_step_ms_ = now;
        }
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
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
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
        DAL::fire_rgb_pulse("all-pixmobs", RgbPulseEvent{
            c.r, c.g, c.b,
            pixmob::T_32_MS, pixmob::T_32_MS, pixmob::T_96_MS,
            pixmob::CHANCE_50});
        last_step_ms_ = now;
        draw_sparkle_screen();
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
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // White Out (sustained white on Btn1, repeatable)
    // -------------------------------------------------------------------------

    void fire_whiteout() {
        // Single command: instant attack, ~2.4 s sustain, ~0.96 s release.
        // The PixMob protocol's Time enum has values up to T_3840_MS so
        // we don't need a multi-command staircase to span 2 s + 1 s.
        DAL::fire_rgb_pulse("all-pixmobs", RgbPulseEvent{
            0xFF, 0xFF, 0xFF,
            pixmob::T_0_MS, pixmob::T_2400_MS, pixmob::T_960_MS,
            pixmob::CHANCE_100});
    }

    void draw_whiteout() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "White Out", WHITE, BLACK, 3});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 60, "A: fire white", WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
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
                if (auto_max_[i] < auto_min_[i] * 4.0f) {
                    auto_max_[i] = auto_min_[i] * 4.0f;
                }
            }
        }

        // Beat detection - same constants as AutonomousMaster so what you
        // see here matches what would fire in Master mode. No IR fires.
        constexpr float kVolumeGate     = 500.0f;
        constexpr float kBaselineAlpha  = 0.02f;
        constexpr float kBeatMultiplier = 2.5f;
        constexpr float kFluxFloor      = 2000.0f;
        constexpr uint32_t kRefractoryMs = 200;

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
        const bool is_beat = flux > audio_baseline_flux_ * kBeatMultiplier
                          && flux > kFluxFloor
                          && (now - audio_last_beat_ms_) > kRefractoryMs;
        if (!is_beat) return;

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
            10, 128, "PWR: back", WHITE, BLACK, 1});
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
                    10, 128, "PWR: back", WHITE, BLACK, 1});
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
                    10, 128, "PWR: back", WHITE, BLACK, 1});
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
                    10, 128, "PWR: back", WHITE, BLACK, 1});
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
