#include "test_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "effects/effects.h"
#include "../dal/drivers/local_driver.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "../dal/drivers/pixmob_ir_driver.h"
#include "pixmob_protocol.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

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

namespace {

struct PaletteColour { uint8_t r, g, b; const char* name; };

constexpr PaletteColour kTestPalette[] = {
    { 0xFF, 0x00, 0x00, "RED"   },
    { 0x00, 0xFF, 0x00, "GREEN" },
    { 0x00, 0x00, 0xFF, "BLUE"  },
    { 0xFF, 0xFF, 0xFF, "WHITE" },
};
constexpr size_t kTestPaletteCount = sizeof(kTestPalette) / sizeof(kTestPalette[0]);

// White only per operator spec; see docs/stickc-history.md.
constexpr PaletteColour kSparkleColour = { 0xFF, 0xFF, 0xFF, "WHITE" };

constexpr uint32_t kPulseStepMs       = 500;    // 2 Hz colour cycle
constexpr uint32_t kFadeStepMs        = 1000;
constexpr uint32_t kRainbowDurationMs = 6000;
constexpr uint32_t kSparkleDurationMs = 10000;
// ~0.9 Hz: gives the 1 s fade (T_0+T_480+T_480 = 960 ms) headroom to
// complete before the next step. See docs/stickc-history.md.
constexpr uint32_t kSparkleStepMs     = 1100;

}  // namespace

constexpr TestMode::MenuItem TestMode::kSubTests[9];

void TestMode::enter() {
    menu_selected_    = 0;
    menu_view_offset_ = 0;
    // Broadcast on the Director-configured channel so any Lume in range
    // renders the test just as it would a real show.
    dal::esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_director_channel());
    // Apply persisted brightness so the local strip doesn't default to
    // 100 % and brown the board out on white pulses.
    DAL::apply_persisted_strip_settings();
    return_to_menu();
}

void TestMode::exit() {
    if (active_test_ == SubTest::AudioLive
     || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
    dal::local_driver_instance()->cancel_pulse();
    dal::esp_now_broadcast_driver_instance()->stop_broadcast();
    active_test_ = SubTest::None;
}

void TestMode::loop_tick() {
    const uint32_t now = millis();
    switch (active_test_) {
        case SubTest::PulseTest:   tick_pulse_or_fade(now, /*fade=*/false); break;
        case SubTest::FadeTest:    tick_pulse_or_fade(now, /*fade=*/true);  break;
        case SubTest::RainbowTest: tick_rainbow(now);                        break;
        case SubTest::SparkleTest: tick_sparkle(now);                        break;
        case SubTest::AudioLive:   tick_audio_live(now);                     break;
        case SubTest::Calibrate:   tick_calibrate(now);                      break;
        case SubTest::PixMobBench: tick_pixmob_bench(now);                   break;
        default: break;
    }
    // Post-pulse status redraw: LocalDriver paints frame-by-frame while
    // a pulse animates, then a final BLACK frame; we overlay the test's
    // status text on the falling edge only (avoid repeatedly overdrawing).
    const bool pulse_active = dal::local_driver_instance()->is_pulse_active();
    if (pulse_was_active_ && !pulse_active) {
        redraw_status_for_active_test();
    }
    pulse_was_active_ = pulse_active;
}

void TestMode::on_button_event(const ButtonPressEvent& ev) {
    if (active_test_ == SubTest::None) handle_button_at_menu(ev);
    else                                handle_button_in_test(ev);
}

void TestMode::on_audio_frame(const AudioFrameEvent& ev) {
    if (active_test_ == SubTest::AudioLive) {
        process_audio_frame(ev);
    } else if (active_test_ == SubTest::Calibrate) {
        on_audio_frame_calibrate(ev);
    }
}

void TestMode::redraw_status_for_active_test() {
    switch (active_test_) {
        case SubTest::PulseTest:    draw_cycle_screen("Pulse");  break;
        case SubTest::FadeTest:     draw_cycle_screen("Fade");   break;
        case SubTest::RainbowTest:  draw_rainbow_screen();       break;
        case SubTest::SparkleTest:  draw_sparkle_screen();       break;
        case SubTest::WhiteOut:     draw_whiteout();             break;
        // AudioLive / Calibrate own their own draw cadence.
        default: break;
    }
}

// -------------------------------------------------------------------------
// Sub-test menu
// -------------------------------------------------------------------------

void TestMode::return_to_menu() {
    if (active_test_ == SubTest::AudioLive
     || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
    // Stop any in-flight pulse before redrawing, or the next loop_tick
    // frame would overdraw the menu list.
    dal::local_driver_instance()->cancel_pulse();
    pulse_was_active_ = false;
    active_test_ = SubTest::None;
    draw_menu();
}

void TestMode::draw_menu() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
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

    // Source_id status label - same bottom-right format as Director Mode.
    auto* drv = dal::esp_now_broadcast_driver_instance();
    char status[16];
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        drv->startup_state(), drv->source_id(), drv->listen_candidate(),
        status, sizeof(status));
    if (n > 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            /*x=*/200, /*y=*/126, /*text=*/status,
            /*fg=*/WHITE, /*bg=*/BLACK, /*size=*/1});
    }
}

void TestMode::update_menu_view_offset() {
    if (menu_selected_ < menu_view_offset_) {
        menu_view_offset_ = menu_selected_;
    } else if (menu_selected_ >= menu_view_offset_ + kMenuVisible) {
        menu_view_offset_ = menu_selected_ - kMenuVisible + 1;
    }
}

void TestMode::handle_button_at_menu(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed
     && ev.kind != ButtonEvent::LongPressed) return;
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
        menu_selected_ = (menu_selected_ + 1) % kSubTestCount;
        // Wrap past bottom resets scroll; otherwise keep cursor onscreen.
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

void TestMode::launch_test(SubTest t) {
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
        case SubTest::WashTest:     enter_wash_test();                    break;
        case SubTest::PixMobBench:  enter_pixmob_bench();                 break;
        default: break;
    }
}

void TestMode::handle_button_in_test(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed
     && ev.kind != ButtonEvent::LongPressed) return;

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
        case SubTest::WashTest:
            handle_button_wash_test(ev);
            break;
        case SubTest::PixMobBench:
            handle_button_pixmob_bench(ev);
            break;
        default:
            break;
    }
}

// -------------------------------------------------------------------------
// Pulse / Fade Test (1 Hz cycle through palette)
// -------------------------------------------------------------------------

void TestMode::enter_pulse_or_fade(bool fade) {
    step_index_   = 0;
    last_step_ms_ = 0;
    draw_cycle_screen(fade ? "Fade" : "Pulse");
    fire_cycle_step(fade);
}

void TestMode::tick_pulse_or_fade(uint32_t now, bool fade) {
    if (now - last_step_ms_ < (fade ? kFadeStepMs : kPulseStepMs)) return;
    step_index_   = (step_index_ + 1) % kTestPaletteCount;
    fire_cycle_step(fade);
    // Status text redraws via the post-pulse hook in loop_tick.
}

void TestMode::fire_cycle_step(bool fade) {
    const auto& c = kTestPalette[step_index_];
    const pixmob::Time attack  = fade ? pixmob::T_192_MS : pixmob::T_32_MS;
    const pixmob::Time sustain = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
    const pixmob::Time release = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
    const RgbPulseEvent ev{
        c.r, c.g, c.b, attack, sustain, release, pixmob::CHANCE_100};
    DAL::render_fx("00:00", ev);
    last_step_ms_ = millis();
}

void TestMode::draw_cycle_screen(const char* title) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, title, WHITE, BLACK, 3});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Colour: %s",
                  kTestPalette[step_index_].name);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 50, buf, WHITE, BLACK, 2});
    const bool is_fade = title && title[0] == 'F';
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 100, is_fade ? "1 Hz auto" : "2 Hz auto", WHITE, BLACK, 2});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B-hold: back", WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// Rainbow Test (6 sec via Rainbow effect)
// -------------------------------------------------------------------------

void TestMode::enter_rainbow() {
    rainbow_hue_         = 0.0f;
    rainbow_last_step_ms_ = 0;
    draw_rainbow_screen();
}

void TestMode::tick_rainbow(uint32_t now) {
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

    // T_192 sustain gives ~92 ms overlap with the 100 ms step - above
    // the Tildagon perimeter's MicroPython poll baseline so envelopes
    // never expire between renders. See docs/stickc-history.md.
    const RgbPulseEvent ev{
        r, g, b,
        pixmob::T_0_MS, pixmob::T_192_MS, pixmob::T_0_MS,
        pixmob::CHANCE_100};
    DAL::render_fx("00:00", ev);
}

void TestMode::draw_rainbow_screen() {
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

void TestMode::enter_sparkle() {
    last_step_ms_ = 0;
    draw_sparkle_screen();
}

void TestMode::tick_sparkle(uint32_t now) {
    if (now - test_start_ms_ >= kSparkleDurationMs) {
        return_to_menu();
        return;
    }
    if (now - last_step_ms_ < kSparkleStepMs) return;

    // ~1 s fade envelope (960 ms total) + CHANCE_32 = "lingering
    // twinkles" per operator spec.
    const auto& c = kSparkleColour;
    const RgbPulseEvent ev{
        c.r, c.g, c.b,
        pixmob::T_0_MS, pixmob::T_480_MS, pixmob::T_480_MS,
        pixmob::CHANCE_32};
    DAL::render_fx("00:00", ev);
    last_step_ms_ = now;
}

void TestMode::draw_sparkle_screen() {
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

void TestMode::fire_whiteout() {
    const RgbPulseEvent ev{
        0xFF, 0xFF, 0xFF,
        pixmob::T_0_MS, pixmob::T_2400_MS, pixmob::T_960_MS,
        pixmob::CHANCE_100};
    DAL::render_fx("00:00", ev);
}

void TestMode::draw_whiteout() {
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

void TestMode::enter_audio_live() {
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

void TestMode::process_audio_frame(const AudioFrameEvent& ev) {
    audio_bass_   = ev.bass_energy;
    audio_mid_    = ev.mid_energy;
    audio_treble_ = ev.treble_energy;
    audio_rms_    = ev.overall_rms;

    auto& s_calibration = persistence::current_calibration();

    // Auto-cal: adapt per-band min/max with a slow leak (~0.3 %/s at
    // 30 Hz frame rate) so the bars track room dynamics.
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
            // Floor at 5 octaves (32x) - the S3's flatter ES8311 codec
            // noise floor compressed the range and made the bars
            // oscillate wildly on any input. See docs/stickc-history.md.
            if (auto_max_[i] < auto_min_[i] * 32.0f) {
                auto_max_[i] = auto_min_[i] * 32.0f;
            }
        }
    }

    // Beat firing is driven by the DAL analyser's BeatDetector so this
    // view matches Director mode. The flux/baseline tracking below is
    // only for the diagnostic meter strip.
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

void TestMode::update_audio_bpm() {
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

void TestMode::tick_audio_live(uint32_t now) {
    // ~20 Hz keeps bars smooth without saturating display refresh.
    if (now - last_step_ms_ < 50) return;
    last_step_ms_ = now;
    draw_audio_live_dynamic();
}

void TestMode::draw_audio_live_static() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 2, "Audio Live", WHITE, BLACK, 1});
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

void TestMode::draw_audio_live_dynamic() {
    auto& s_calibration = persistence::current_calibration();
    // Log2 scaling: band sums span several decades (mid = 57 bins,
    // treble = 191). Floor/ceil from Calibrate or rolling auto-cal.
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
        // Right-pad with trailing spaces to overwrite previous longer
        // strings cleanly.
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

    const bool active = audio_beat_flash_until_ > millis();
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        120, 114, active ? "BEAT!" : "     ",
        active ? YELLOW : BLACK, BLACK, 1});
}

// -------------------------------------------------------------------------
// Calibrate (sound-check workflow + auto-cal toggle, persists to NVS)
// -------------------------------------------------------------------------
// Manual sound check: 3 s silence -> rolling min into floors, then 10 s
// music -> rolling max into ceilings. Auto mode uses rolling per-band
// min/max instead so bars stay readable when the room's noise floor
// shifts mid-show.

void TestMode::enter_calibrate() {
    cal_state_           = CalState::Menu;
    cal_phase_start_ms_  = 0;
    cal_last_redraw_ms_  = 0;
    DAL::start_audio_input("local", 16000, 512);
    draw_calibrate();
}

void TestMode::on_audio_frame_calibrate(const AudioFrameEvent& ev) {
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

void TestMode::tick_calibrate(uint32_t now) {
    auto& s_calibration = persistence::current_calibration();
    switch (cal_state_) {
        case CalState::BaselineCapture:
            if (now - cal_phase_start_ms_ >= kCalBaselineMs) {
                // +0.3 log2 headroom so quiet readings sit just above 0 %.
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
                persistence::save_calibration(s_calibration);
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

void TestMode::handle_button_calibrate(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed) return;
    auto& s_calibration = persistence::current_calibration();
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
        persistence::save_calibration(s_calibration);
        draw_calibrate();
    }
}

void TestMode::draw_calibrate() {
    auto& s_calibration = persistence::current_calibration();
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

// -------------------------------------------------------------------------
// Wash Test. Btn2 cycles Fire / Cancel / Pulse-over-wash; Btn1 broadcasts
// to 00:00 (every class, every group). LCD is UI here, not a light surface
// - verify visually on a second Stick in Lume mode or a Tildagon.
// -------------------------------------------------------------------------

void TestMode::enter_wash_test() {
    step_index_   = 0;     // 0 = Fire, 1 = Cancel, 2 = Pulse over wash
    last_step_ms_ = 0;     // no "Sent!" flash yet
    draw_wash_test();
}

void TestMode::handle_button_wash_test(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed) return;
    if (ev.id == ButtonId::Btn2) {
        step_index_ = (step_index_ + 1) % kWashTestItemCount;
        draw_wash_test();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        if (step_index_ == 0) {
            // Orange <-> purple 5 s drift, infinite ttl (cancelled explicitly),
            // pulse_response=1 accepts LIGHT_PULSE as overlay.
            LightWashEvent w{};
            w.r1 = 255; w.g1 =  60; w.b1 =   0;
            w.r2 = 120; w.g2 =  30; w.b2 = 200;
            w.attack         = 20;
            w.release        = 10;
            w.intensity      = 200;
            w.cycle_ms       = 5000;
            w.ttl_seconds    = 0;
            w.pulse_response = 1;
            DAL::render_wash("00:00", w);
        } else if (step_index_ == 1) {
            DAL::render_wash_end("00:00", /*release_time=*/10);   // 1.0 s
        } else {
            // Pulse-over-wash: white sparkle. PixMob routes it as
            // TwoColors(white, current_wash_rgb) - flashes white then
            // returns to wash.
            RgbPulseEvent p{};
            p.r = 255; p.g = 255; p.b = 255;
            p.attack  = pixmob::T_0_MS;
            p.sustain = pixmob::T_32_MS;
            p.release = pixmob::T_96_MS;
            p.chance  = pixmob::CHANCE_100;
            DAL::render_fx("00:00", p);
        }
        last_step_ms_ = millis();
        draw_wash_test();
    }
}

void TestMode::draw_wash_test() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Wash Test", WHITE, BLACK, 2});

    const char* lines[kWashTestItemCount] = {
        "Fire test wash",
        "Cancel wash",
        "Pulse over wash",
    };
    for (size_t i = 0; i < kWashTestItemCount; ++i) {
        const bool sel = (i == step_index_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 35 + (int)i * 18, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 80, "broadcast to 00:00",
        WHITE, BLACK, 1});

    if (last_step_ms_ != 0 && (millis() - last_step_ms_) < kWashConfirmFlashMs) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 100, "Sent!", GREEN, BLACK, 2});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: fire  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// PixMob Bench. Six sequential IR experiments that drive the IR HAL
// directly (bypassing DAL::render_fx). Btn2 cycles tests; Btn1 advances
// within a test. Operator reads the bracelet's behaviour by hand.
// -------------------------------------------------------------------------

namespace {

// Any 9-byte PixMob command fits well under 80 pulses on the wire.
constexpr size_t kPMobPulseBufSize = 80;

}  // namespace

void TestMode::pmob_fire_set_background(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t buf[kPMobPulseBufSize];
    const size_t n = pixmob::buildSetColor(
        buf, kPMobPulseBufSize,
        r, g, b,
        /*profileId=*/0,
        /*isBackground=*/true,
        /*skipDisplay=*/false,
        /*restrictGroupId=*/0);
    if (n == 0) return;
    auto* drv = dal::pixmob_ir_driver_instance();
    if (drv) drv->transmit(buf, n);
}

void TestMode::pmob_fire_single_color(uint8_t r, uint8_t g, uint8_t b,
                                       uint8_t attack, uint8_t sustain,
                                       uint8_t release) {
    uint16_t buf[kPMobPulseBufSize];
    const size_t n = pixmob::buildSingleColor(
        buf, kPMobPulseBufSize,
        r, g, b,
        static_cast<pixmob::Time>(attack),
        static_cast<pixmob::Time>(sustain),
        static_cast<pixmob::Time>(release),
        pixmob::CHANCE_100,
        /*restrictGroupId=*/0);
    if (n == 0) return;
    auto* drv = dal::pixmob_ir_driver_instance();
    if (drv) drv->transmit(buf, n);
}

void TestMode::enter_pixmob_bench() {
    pmob_test_         = PMobTest::Test1Persistence;
    pmob_step_         = 0;
    pmob_last_fire_ms_ = 0;
    draw_pixmob_bench();
}

void TestMode::handle_button_pixmob_bench(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed) return;
    if (ev.id == ButtonId::Btn2) {
        // Cycle tests; reset step and stop any in-flight T5 auto-refresh.
        uint8_t next = static_cast<uint8_t>(pmob_test_) + 1;
        if (next > static_cast<uint8_t>(PMobTest::Test6TwoColors)) next = 0;
        pmob_test_ = static_cast<PMobTest>(next);
        pmob_step_ = 0;
        pmob_last_fire_ms_ = 0;
        pmob_t5_index_ = 0;
        draw_pixmob_bench();
        return;
    }
    if (ev.id != ButtonId::Btn1) return;

    switch (pmob_test_) {
        case PMobTest::Test1Persistence:
            pmob_fire_set_background(200, 0, 200);
            pmob_step_ = 1;
            break;
        case PMobTest::Test2Sleep:
            if (pmob_step_ == 0) {
                pmob_fire_set_background(200, 0, 200);
                pmob_step_ = 1;
            } else {
                // No-op SingleColor to wake; if background state
                // survived sleep the bracelet returns to purple.
                pmob_fire_single_color(
                    0, 0, 0,
                    pixmob::T_0_MS, pixmob::T_0_MS, pixmob::T_0_MS);
                pmob_step_ = 0;
            }
            break;
        case PMobTest::Test3Cancel:
            if (pmob_step_ == 0) {
                pmob_fire_set_background(255, 0, 0);
                pmob_step_ = 1;
            } else {
                pmob_fire_set_background(0, 0, 0);
                pmob_step_ = 0;
            }
            break;
        case PMobTest::Test4EnvelopeSweep: {
            // Attack + release at T_0 so visible duration is dominated
            // by the sustain bucket the operator can time.
            const uint8_t sustain_bucket = pmob_step_ & 0x07;
            pmob_fire_single_color(
                255, 255, 255,
                pixmob::T_0_MS, sustain_bucket, pixmob::T_0_MS);
            pmob_step_ = (pmob_step_ + 1) & 0x07;
            break;
        }
        case PMobTest::Test5AutoRefresh:
            pmob_t5_index_ = (pmob_t5_index_ + 1) % kPMobT5IntervalCount;
            if (pmob_t5_index_ != 0) {
                // Fire immediately so the operator doesn't wait a full
                // interval before seeing anything; tick handles cadence.
                pmob_fire_single_color(
                    255, 255, 255,
                    pixmob::T_0_MS, pixmob::T_3840_MS, pixmob::T_0_MS);
                pmob_t5_last_refresh_ms_ = millis();
            }
            break;
        case PMobTest::Test6TwoColors: {
            // TwoColors isolation: bypass wash state so we can tell
            // whether the bracelet honours TwoColors at all
            // (precondition for sparkle-on-wash during shows).
            uint16_t buf[kPMobPulseBufSize];
            const size_t n = pixmob::buildTwoColors(
                buf, kPMobPulseBufSize,
                255, 0, 0,     // red sparkle
                0, 0, 255);    // blue tail (~384 ms hold)
            if (n != 0) {
                auto* drv = dal::pixmob_ir_driver_instance();
                if (drv) drv->transmit(buf, n);
            }
            break;
        }
    }
    pmob_last_fire_ms_ = millis();
    draw_pixmob_bench();
}

void TestMode::tick_pixmob_bench(uint32_t now) {
    if (pmob_test_ != PMobTest::Test5AutoRefresh) return;
    if (pmob_t5_index_ == 0) return;
    const uint32_t interval = kPMobT5Intervals[pmob_t5_index_];
    if ((now - pmob_t5_last_refresh_ms_) < interval) return;
    // Square envelope so the new refresh pre-empts the previous sustain
    // cleanly. See docs/stickc-history.md.
    pmob_fire_single_color(
        255, 255, 255,
        pixmob::T_0_MS, pixmob::T_3840_MS, pixmob::T_0_MS);
    pmob_t5_last_refresh_ms_ = now;
    // Auto-fires deliberately don't touch pmob_last_fire_ms_ - operator
    // watches the bracelet, a "Sent!" flash every cycle would distract.
}

void TestMode::draw_pixmob_bench() {
    using dal::DisplayClearEvent;
    using dal::DisplayShowTextEvent;
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "PMob Bench", WHITE, BLACK, 2});

    const char* test_label = "?";
    switch (pmob_test_) {
        case PMobTest::Test1Persistence:    test_label = "T1 Persist";   break;
        case PMobTest::Test2Sleep:          test_label = "T2 Sleep";     break;
        case PMobTest::Test3Cancel:         test_label = "T3 Cancel";    break;
        case PMobTest::Test4EnvelopeSweep:  test_label = "T4 Envelope";  break;
        case PMobTest::Test5AutoRefresh:    test_label = "T5 Refresh";   break;
        case PMobTest::Test6TwoColors:      test_label = "T6 TwoColor";  break;
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 30, test_label, YELLOW, BLACK, 2});

    // T4 builds line strings dynamically so the LCD shows which bucket
    // is ABOUT to fire (line1 = name, line2 = nominal ms).
    char t4_line1[24];
    char t4_line2[24];
    const char* line1 = "";
    const char* line2 = "";
    switch (pmob_test_) {
        case PMobTest::Test1Persistence:
            line1 = (pmob_step_ == 0)
                ? "A: set purple"
                : "Pull battery 30s,";
            line2 = (pmob_step_ == 0)
                ? "background"
                : "replace, observe";
            break;
        case PMobTest::Test2Sleep:
            line1 = (pmob_step_ == 0)
                ? "A: set purple,"
                : "A: wake. Did it";
            line2 = (pmob_step_ == 0)
                ? "wait til dark"
                : "come back purple?";
            break;
        case PMobTest::Test3Cancel:
            line1 = (pmob_step_ == 0)
                ? "A: red. then A"
                : "Black instantly?";
            line2 = (pmob_step_ == 0)
                ? "for black."
                : "Fades? Ignored?";
            break;
        case PMobTest::Test4EnvelopeSweep: {
            static constexpr const char* kBucketLabels[8] = {
                "T_0_MS",   "T_32_MS",   "T_96_MS",   "T_192_MS",
                "T_480_MS", "T_960_MS",  "T_2400_MS", "T_3840_MS",
            };
            static constexpr uint16_t kBucketMs[8] = {
                0, 32, 96, 192, 480, 960, 2400, 3840,
            };
            const uint8_t idx = pmob_step_ & 0x07;
            std::snprintf(t4_line1, sizeof(t4_line1),
                          "Next: %s", kBucketLabels[idx]);
            std::snprintf(t4_line2, sizeof(t4_line2),
                          "%u ms (eyeball it)", (unsigned)kBucketMs[idx]);
            line1 = t4_line1;
            line2 = t4_line2;
            break;
        }
        case PMobTest::Test5AutoRefresh: {
            if (pmob_t5_index_ == 0) {
                line1 = "OFF";
                line2 = "A: cycle 3000/2500/";
            } else {
                std::snprintf(t4_line1, sizeof(t4_line1),
                              "Refresh: %u ms",
                              (unsigned)kPMobT5Intervals[pmob_t5_index_]);
                line1 = t4_line1;
                line2 = "A: next interval";
            }
            break;
        }
        case PMobTest::Test6TwoColors: {
            line1 = "A: fire TwoColors";
            line2 = "red flash + blue hold";
            break;
        }
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 60, line1, WHITE, BLACK, 1});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 75, line2, WHITE, BLACK, 1});

    if (pmob_last_fire_ms_ != 0
     && (millis() - pmob_last_fire_ms_) < kPMobConfirmFlashMs) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 95, "Sent!", GREEN, BLACK, 2});
    }

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: next test  B-hold: back",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
