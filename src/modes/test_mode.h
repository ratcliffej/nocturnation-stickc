// TestMode - the §8.5 test-mode catalogue plus a Set Group ID helper.
//
// Two-level UI: enter Test Mode -> sub-test list. Btn2 cycles, Btn1 launches.
// Inside a sub-test, button bindings are sub-test-specific and B-hold
// returns to the sub-test list (not the main menu); a second B-hold from
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

#pragma once

#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"          // public header in include/

namespace nocturnation {
namespace modes {

class TestMode : public Mode {
public:
    ModeId id() const override { return ModeId::Test; }
    const char* name() const override { return "Test"; }

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;
    void on_audio_frame(const dal::AudioFrameEvent& ev) override;

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

    // Audio Live state.
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

    // Auto-cal rolling per-band min/max with leak.
    float    auto_min_[4]          = { 1.0e9f, 1.0e9f, 1.0e9f, 1.0e9f };
    float    auto_max_[4]          = { 1.0f,   1.0f,   1.0f,   1.0f   };

    // Rainbow Test inline hue cycling state.
    static constexpr float    kRainbowCycleHz       = 0.5f;
    static constexpr float    kRainbowBrightness    = 1.0f;
    static constexpr uint16_t kRainbowStepIntervalMs = 50;
    float    rainbow_hue_         = 0.0f;
    uint32_t rainbow_last_step_ms_ = 0;

    // Test mode broadcasts on the same channel AutonomousMaster uses
    // (NVS-configured: 1 hobby / 11 show / 6 custom). The radio itself
    // lives in EspNowBroadcastDriver - this mode just starts/stops it
    // in enter/exit and hits the wire via render_fx.

    bool pulse_was_active_ = false;

    void redraw_status_for_active_test();

    // Sub-test menu.
    void return_to_menu();
    static constexpr size_t kMenuVisible = 8;
    void draw_menu();
    void update_menu_view_offset();
    void handle_button_at_menu(const dal::ButtonPressEvent& ev);

    // Sub-test launch + per-sub-test button handling.
    void launch_test(SubTest t);
    void handle_button_in_test(const dal::ButtonPressEvent& ev);

    // Pulse / Fade Test.
    void enter_pulse_or_fade(bool fade);
    void tick_pulse_or_fade(uint32_t now, bool fade);
    void fire_cycle_step(bool fade);
    void draw_cycle_screen(const char* title);

    // Rainbow Test.
    void enter_rainbow();
    void tick_rainbow(uint32_t now);
    void draw_rainbow_screen();

    // Sparkle Test.
    void enter_sparkle();
    void tick_sparkle(uint32_t now);
    void draw_sparkle_screen();

    // White Out.
    void fire_whiteout();
    void draw_whiteout();

    // Audio Live.
    void enter_audio_live();
    void process_audio_frame(const dal::AudioFrameEvent& ev);
    void update_audio_bpm();
    void tick_audio_live(uint32_t now);
    void draw_audio_live_static();
    void draw_audio_live_dynamic();

    // Calibrate.
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

    void enter_calibrate();
    void on_audio_frame_calibrate(const dal::AudioFrameEvent& ev);
    void tick_calibrate(uint32_t now);
    void handle_button_calibrate(const dal::ButtonPressEvent& ev);
    void draw_calibrate();
};

}  // namespace modes
}  // namespace nocturnation
