// TestMode - the §8.5 test-mode catalogue.
//
// Two-level UI: Test Mode -> sub-test list. Btn2 cycles, Btn1 launches.
// Inside a sub-test, button bindings are sub-test-specific; B-hold pops
// back to the sub-test list, a second B-hold returns to the main menu.

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
        WashTest,
        PixMobBench,
    };

    struct MenuItem { SubTest test; const char* label; };
    static constexpr MenuItem kSubTests[9] = {
        { SubTest::PulseTest,    "Pulse"        },
        { SubTest::FadeTest,     "Fade"         },
        { SubTest::RainbowTest,  "Rainbow"      },
        { SubTest::SparkleTest,  "Sparkle"      },
        { SubTest::WhiteOut,     "White Out"    },
        { SubTest::AudioLive,    "Audio Live"   },
        { SubTest::Calibrate,    "Calibrate"    },
        { SubTest::WashTest,     "Wash Test"    },
        { SubTest::PixMobBench,  "PMob Bench"   },
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
    // 100 ms step chosen to clear the Tildagon perimeter's Full-mode
    // Harding cap with headroom; sustain (T_192) overlaps by ~92 ms so
    // envelopes never expire before the next pulse arrives. See
    // docs/stickc-history.md.
    static constexpr uint16_t kRainbowStepIntervalMs = 100;
    float    rainbow_hue_         = 0.0f;
    uint32_t rainbow_last_step_ms_ = 0;

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

    // Wash Test. Three-item sub-screen (Fire wash / Cancel wash / Pulse
    // over wash). step_index_ doubles as the cursor; last_step_ms_
    // stamps the transmit so the "Sent!" flash lingers.
    static constexpr size_t   kWashTestItemCount = 3;
    static constexpr uint32_t kWashConfirmFlashMs = 800;
    void enter_wash_test();
    void handle_button_wash_test(const dal::ButtonPressEvent& ev);
    void draw_wash_test();

    // PixMob Bench: bench-only IR experiments (persistence, auto-sleep,
    // cancel, envelope sweep, auto-refresh, TwoColors isolation).
    // Drives the IR HAL directly with buildSetColor / buildSingleColor
    // pulse trains, bypassing DAL::render_fx.
    enum class PMobTest : uint8_t {
        Test1Persistence = 0,
        Test2Sleep,
        Test3Cancel,
        Test4EnvelopeSweep,   // pmob_step_ = sustain bucket 0..7
        Test5AutoRefresh,     // pmob_t5_index_ selects refresh interval
        Test6TwoColors,
    };
    static constexpr uint32_t kPMobConfirmFlashMs   = 800;

    // T5 interval cycle: index 0 = OFF, others = ms between refreshes.
    static constexpr uint16_t kPMobT5Intervals[6] = {
        0, 3000, 2500, 2000, 1500, 1000,
    };
    static constexpr uint8_t kPMobT5IntervalCount = 6;

    PMobTest pmob_test_              = PMobTest::Test1Persistence;
    uint8_t  pmob_step_              = 0;
    uint32_t pmob_last_fire_ms_      = 0;
    uint8_t  pmob_t5_index_          = 0;   // 0 = off; >0 -> kPMobT5Intervals[idx]
    uint32_t pmob_t5_last_refresh_ms_ = 0;

    void enter_pixmob_bench();
    void tick_pixmob_bench(uint32_t now);
    void handle_button_pixmob_bench(const dal::ButtonPressEvent& ev);
    void draw_pixmob_bench();

    // Bench IR helpers - bypass DAL::render_fx to exercise the pulse
    // trains verbatim.
    void pmob_fire_set_background(uint8_t r, uint8_t g, uint8_t b);
    void pmob_fire_single_color(uint8_t r, uint8_t g, uint8_t b,
                                 uint8_t attack, uint8_t sustain, uint8_t release);
};

}  // namespace modes
}  // namespace nocturnation
