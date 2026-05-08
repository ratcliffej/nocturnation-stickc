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
#else
ModeId load_last_runtime_mode() { return kDefaultRuntimeMode; }
void   save_last_runtime_mode(ModeId)  {}
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
        start_ms_ = millis();
        last_drawn_seconds_ = 0xFF;     // force first draw
        draw();
    }

    void loop_tick() override {
        const uint32_t elapsed = millis() - start_ms_;
        if (elapsed >= kBootCountdownMs) {
            ModeMachine::switch_to(s_last_runtime);
            return;
        }
        const uint8_t remaining = (uint8_t)((kBootCountdownMs - elapsed) / 1000) + 1;
        if (remaining != last_drawn_seconds_) {
            last_drawn_seconds_ = remaining;
            draw();
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

    void draw() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 10, "NocturNation", WHITE, BLACK, 3});

        const uint32_t elapsed   = millis() - start_ms_;
        const uint32_t remaining = (kBootCountdownMs > elapsed)
                                       ? (kBootCountdownMs - elapsed) : 0;
        const uint8_t  seconds   = (uint8_t)(remaining / 1000) + 1;
        char buf[40];
        std::snprintf(buf, sizeof(buf),
                      "Default: %s\nin %u s",
                      "Master", (unsigned)seconds);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, buf, WHITE, BLACK, 2});

        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 110, "press any btn for menu", WHITE, BLACK, 1});
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

        char batt[24];
        std::snprintf(batt, sizeof(batt), " Batt: %d%%",
                      DAL::battery_level("local"));
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
// ConfigMode - stub. Full §8.4 config tree lands in Epic 3 UI.
// =============================================================================

class ConfigMode : public Mode {
public:
    ModeId id() const override { return ModeId::Config; }
    const char* name() const override { return "Config"; }

    void enter() override {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 10, "Config", WHITE, BLACK, 3});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, "TBD (Epic 3 UI)", WHITE, BLACK, 2});
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
constexpr PaletteColour kTestPalette[] = {
    { 0xFF, 0x00, 0x00, "RED"   },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
    { 0xFF, 0xFF, 0xFF, "WHITE" },
};
constexpr size_t kTestPaletteCount = sizeof(kTestPalette) / sizeof(kTestPalette[0]);

constexpr uint32_t kPulseStepMs       = 1000;   // 1 Hz colour cycle
constexpr uint32_t kFadeStepMs        = 1000;
constexpr uint32_t kRainbowDurationMs = 6000;
constexpr uint32_t kSparkleDurationMs = 10000;
constexpr uint32_t kSparkleStepMs     = 200;
constexpr uint32_t kConfirmFlashMs    = 800;    // "Sent!" linger time

}  // namespace

class TestMode : public Mode {
public:
    ModeId id() const override { return ModeId::Test; }
    const char* name() const override { return "Test"; }

    void enter() override {
        return_to_menu();
    }

    void exit() override {
        if (active_test_ == SubTest::RainbowTest) rainbow_.exit();
        active_test_ = SubTest::None;
    }

    void loop_tick() override {
        const uint32_t now = millis();
        switch (active_test_) {
            case SubTest::PulseTest:   tick_pulse_or_fade(now, /*fade=*/false); break;
            case SubTest::FadeTest:    tick_pulse_or_fade(now, /*fade=*/true);  break;
            case SubTest::RainbowTest: tick_rainbow(now);                        break;
            case SubTest::SparkleTest: tick_sparkle(now);                        break;
            case SubTest::SetGroupId:  tick_confirm_flash(now);                  break;
            default: break;
        }
    }

    void on_button_event(const ButtonPressEvent& ev) override {
        if (active_test_ == SubTest::None) handle_button_at_menu(ev);
        else                                handle_button_in_test(ev);
    }

private:
    enum class SubTest : uint8_t {
        None = 0,
        PulseTest,
        FadeTest,
        RainbowTest,
        SparkleTest,
        WhiteOut,
        GroupTarget,
        SetGroupId,
    };

    struct MenuItem { SubTest test; const char* label; };
    static constexpr MenuItem kSubTests[7] = {
        { SubTest::PulseTest,   "Pulse"        },
        { SubTest::FadeTest,    "Fade"         },
        { SubTest::RainbowTest, "Rainbow"      },
        { SubTest::SparkleTest, "Sparkle"      },
        { SubTest::WhiteOut,    "White Out"    },
        { SubTest::GroupTarget, "Group Target" },
        { SubTest::SetGroupId,  "Set Group ID" },
    };
    static constexpr size_t kSubTestCount = sizeof(kSubTests) / sizeof(kSubTests[0]);

    SubTest  active_test_   = SubTest::None;
    size_t   menu_selected_ = 0;
    uint8_t  step_index_    = 0;
    uint32_t test_start_ms_ = 0;
    uint32_t last_step_ms_  = 0;
    uint32_t confirm_until_ms_ = 0;
    uint8_t  target_group_  = 1;     // for GroupTarget + SetGroupId

    effects::Rainbow rainbow_{"all-pixmobs", 0.5f, 1.0f};

    // -------------------------------------------------------------------------
    // Sub-test menu
    // -------------------------------------------------------------------------

    void return_to_menu() {
        if (active_test_ == SubTest::RainbowTest) rainbow_.exit();
        active_test_ = SubTest::None;
        draw_menu();
    }

    void draw_menu() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Test Mode", WHITE, BLACK, 2});
        for (size_t i = 0; i < kSubTestCount; ++i) {
            const bool sel = (i == menu_selected_);
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%s %s",
                          sel ? ">" : " ", kSubTests[i].label);
            DAL::fire_display_show_text("local", DisplayShowTextEvent{
                10, 30 + (int)i * 14, buf,
                sel ? YELLOW : WHITE, BLACK, 1});
        }
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "B: cycle  A: launch", WHITE, BLACK, 1});
    }

    void handle_button_at_menu(const ButtonPressEvent& ev) {
        if (ev.kind != ButtonEvent::Pressed
         && ev.kind != ButtonEvent::LongPressed) return;
        if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
            menu_selected_ = (menu_selected_ + 1) % kSubTestCount;
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
        target_group_    = 1;
        confirm_until_ms_= 0;
        test_start_ms_   = millis();
        last_step_ms_    = 0;
        switch (t) {
            case SubTest::PulseTest:    enter_pulse_or_fade(/*fade=*/false); break;
            case SubTest::FadeTest:     enter_pulse_or_fade(/*fade=*/true);  break;
            case SubTest::RainbowTest:  enter_rainbow();                      break;
            case SubTest::SparkleTest:  enter_sparkle();                      break;
            case SubTest::WhiteOut:     draw_whiteout();                      break;
            case SubTest::GroupTarget:  draw_group_target();                  break;
            case SubTest::SetGroupId:   draw_set_group_id();                  break;
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
            case SubTest::GroupTarget:
                if (ev.id == ButtonId::Btn1) fire_group_target_advance();
                break;
            case SubTest::SetGroupId:
                if (ev.id == ButtonId::Btn1) send_set_group_id();
                else if (ev.id == ButtonId::Btn2) cycle_target_group();
                break;
            default:
                // Continuous tests (Pulse/Fade/Rainbow/Sparkle) ignore button
                // presses other than the back gesture. Btn1 could be wired to
                // "skip to next colour" later if useful.
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
        const auto& c = kTestPalette[std::rand() % kTestPaletteCount];
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
        DAL::fire_rgb_pulse("all-pixmobs", RgbPulseEvent{
            0xFF, 0xFF, 0xFF,
            pixmob::T_32_MS, pixmob::T_192_MS, pixmob::T_192_MS,
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
    // Group Targeting Test (Btn1 fires + advances through groups 1-5)
    // -------------------------------------------------------------------------

    void fire_group_target_advance() {
        char target[16];
        std::snprintf(target, sizeof(target), "group-%u", (unsigned)target_group_);
        DAL::fire_rgb_pulse(target, RgbPulseEvent{
            0xFF, 0xFF, 0xFF,
            pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS,
            pixmob::CHANCE_100});
        target_group_ = (target_group_ % 5) + 1;
        draw_group_target();
    }

    void draw_group_target() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Group Target", WHITE, BLACK, 2});
        char buf[24];
        std::snprintf(buf, sizeof(buf), "Next: group %u", (unsigned)target_group_);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 50, buf, WHITE, BLACK, 2});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 90, "A: fire + advance", WHITE, BLACK, 1});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 128, "PWR-hold: back", WHITE, BLACK, 1});
    }

    // -------------------------------------------------------------------------
    // Set Group ID (bracelet setup helper)
    // -------------------------------------------------------------------------

    void cycle_target_group() {
        target_group_ = (target_group_ % 5) + 1;
        draw_set_group_id();
    }

    void send_set_group_id() {
        // Target name is irrelevant - the AssignDeviceGroup dispatch ignores
        // device group_id and uses the event's new_group_id payload.
        const bool ok = DAL::fire_assign_device_group("all-pixmobs",
                            AssignDeviceGroupEvent{target_group_});
        if (ok) confirm_until_ms_ = millis() + kConfirmFlashMs;
        draw_set_group_id();
    }

    void tick_confirm_flash(uint32_t now) {
        if (confirm_until_ms_ != 0 && now >= confirm_until_ms_) {
            confirm_until_ms_ = 0;
            draw_set_group_id();
        }
    }

    void draw_set_group_id() {
        DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 5, "Set Group ID", WHITE, BLACK, 2});
        char buf[24];
        std::snprintf(buf, sizeof(buf), "New group: %u", (unsigned)target_group_);
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
