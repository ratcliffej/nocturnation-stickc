// TestMode implementation.

#include "test_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "effects/effects.h"
#include "../dal/drivers/local_driver.h"               // for cancel_pulse / is_pulse_active
#include "../dal/drivers/espnow_broadcast_driver.h"    // for start/stop_broadcast
#include "../dal/drivers/pixmob_ir_driver.h"           // for direct IR transmit in PMob Bench
#include "pixmob_protocol.h"                            // for buildSetColor / buildSingleColor

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

// Sparkle test uses white only (operator request: the random colours
// felt too busy; white sparkle reads as crowd-shimmer).
constexpr PaletteColour kSparkleColour = { 0xFF, 0xFF, 0xFF, "WHITE" };

constexpr uint32_t kPulseStepMs       = 500;    // 2 Hz colour cycle
constexpr uint32_t kFadeStepMs        = 1000;
constexpr uint32_t kRainbowDurationMs = 6000;
constexpr uint32_t kSparkleDurationMs = 10000;
// Step interval gives the 1 s fade room to complete before the next
// step cuts it short. Fade total = T_0 + T_480 + T_480 = 960 ms after
// the main pulse arrives. 1100 ms step leaves ~140 ms safety gap;
// cadence is ~0.9 Hz which reads as "lingering twinkles" against the
// random-chance fire.
constexpr uint32_t kSparkleStepMs     = 1100;

}  // namespace

constexpr TestMode::MenuItem TestMode::kSubTests[9];

void TestMode::enter() {
    menu_selected_    = 0;
    menu_view_offset_ = 0;
    // Same channel as Director (NVS-configured). Test fires
    // broadcast over ESP-NOW so any Lume on the configured show
    // channel renders the colour and forwards IR to its own bracelets,
    // just like during a real show.
    dal::esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_director_channel());
    // Pulse / Whiteout tests render through the local LED strip too.
    // Apply persisted brightness so the Config-menu setting is
    // honoured here, not just in LumeMode - otherwise the strip
    // defaults to 100 % and runs the board into brownout on white.
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
    // Redundant-retransmit pump + 1 Hz skip-if-recent heartbeat now run
    // from EspNowBroadcastDriver::loop_tick (which DAL::loop_tick drives
    // every main-loop iteration). Test mode keeps broadcasting through
    // gaps between sub-test fires because the driver stays active across
    // the whole mode lifetime - start_broadcast lives in enter().
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
        // AudioLive and Calibrate own their own draw cadence; no
        // post-pulse redraw needed (they don't drive RgbPulse).
        default: break;
    }
}

// -------------------------------------------------------------------------
// Sub-test menu
// -------------------------------------------------------------------------

void TestMode::return_to_menu() {
    if (active_test_ == SubTest::AudioLive
     || active_test_ == SubTest::Calibrate)   DAL::stop_audio_input("local");
    // Stop any in-flight pulse animation before redrawing the menu;
    // otherwise the next loop_tick frame would overdraw the menu list.
    dal::local_driver_instance()->cancel_pulse();
    pulse_was_active_ = false;
    active_test_ = SubTest::None;
    draw_menu();
}

void TestMode::draw_menu() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    // 8 items at size 2 + 16 px line spacing = 128 px content + 4 px
    // top margin = 132 px, just inside the 135 px display. When the
    // sub-test list grows past 8, the cursor scrolls the visible
    // window via menu_view_offset_. Title and bottom hint dropped to
    // make room; B-hold returns to the main mode menu (handled in
    // handle_button_at_menu via Btn2 LongPressed), and B/A
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

    // Source_id status label (Epic 5.5 B5). Same bottom-right placement
    // and format as Director Mode. TestMode also broadcasts on the
    // configured Director channel, so the ID is meaningful here too.
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
        case SubTest::WashTest:
            handle_button_wash_test(ev);
            break;
        case SubTest::PixMobBench:
            handle_button_pixmob_bench(ev);
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
    // Status text redraws via the post-pulse hook in loop_tick - if we
    // redraw here we'd overdraw the pulse animation immediately.
}

void TestMode::fire_cycle_step(bool fade) {
    const auto& c = kTestPalette[step_index_];
    const pixmob::Time attack  = fade ? pixmob::T_192_MS : pixmob::T_32_MS;
    const pixmob::Time sustain = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
    const pixmob::Time release = fade ? pixmob::T_192_MS : pixmob::T_96_MS;
    const RgbPulseEvent ev{
        c.r, c.g, c.b, attack, sustain, release, pixmob::CHANCE_100};
    // Single transmit call: dispatch_output_class_group fans out to
    // ESP-NOW broadcast (Lumes), Director IR (Light-class loopback),
    // and Director screen (Screen-class loopback / Config > Display >
    // Pulse gated). Same path Shows use.
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

    // Envelope: attack=0, sustain=192, release=0. 100 ms step + 192 ms
    // sustain = 92 ms overlap - well above the Tildagon perimeter's
    // ~85 ms MicroPython poll baseline (with occasional ~160 ms
    // outliers). Without this overlap, envelope 1 expires ~96 ms into
    // its life, and if the next Tildagon render tick catches that
    // window before pulse 2 has been drained from the espnow queue,
    // the perimeter clears the envelope and paints black (visible as
    // a strobe gap in the rainbow sweep). Bracelet + Atom LED strip
    // effect unchanged - both handled fast pulses fine before.
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

    // Operator spec: white, ~1 s fade envelope (T_0 + T_480 + T_480
    // = 960 ms total), ~0.9 Hz fire cadence (kSparkleStepMs above -
    // slowed from 2 Hz so the 1 s fade completes before the next
    // step cuts it short), ~32 % per-bracelet chance (CHANCE_32,
    // one step up the chance ladder from CHANCE_16).
    const auto& c = kSparkleColour;
    const RgbPulseEvent ev{
        c.r, c.g, c.b,
        pixmob::T_0_MS, pixmob::T_480_MS, pixmob::T_480_MS,
        pixmob::CHANCE_32};
    DAL::render_fx("00:00", ev);
    last_step_ms_ = now;
    // Status text redraws via the post-pulse hook in loop_tick.
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
    // Main white pulse: instant attack, ~2.4 s sustain, ~0.96 s release.
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
    // would fire in Director mode. The flux/baseline tracking below
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
    // Refresh dynamic elements at ~20 Hz to keep bars smooth without
    // saturating IR-bus or display refresh.
    if (now - last_step_ms_ < 50) return;
    last_step_ms_ = now;
    draw_audio_live_dynamic();
}

void TestMode::draw_audio_live_static() {
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

void TestMode::draw_audio_live_dynamic() {
    auto& s_calibration = persistence::current_calibration();
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
// Wash Test (Epic 6C). Fire / Cancel picker; Btn2 cycles, Btn1 broadcasts.
//
// Both actions target "00:00" (every class, every group) so any capable
// Lume in range reacts. The Stick's own LCD won't show the wash - the LCD
// is UI in Test mode, not a light surface (that's Lume-only). To verify
// visually, run a second Stick in Lume mode or a Tildagon Lume.
// -------------------------------------------------------------------------

void TestMode::enter_wash_test() {
    step_index_   = 0;     // 0 = Fire selected, 1 = Cancel
    last_step_ms_ = 0;     // no transmit yet -> no "Sent!" flash
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
            // Fire: orange<->purple 5 s drift, 2.0 s attack, 1.0 s release,
            // intensity 200, ttl=0 (infinite; cancelled explicitly),
            // pulse_response=1 (accept LIGHT_PULSE as overlay).
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
            // Pulse over wash. Bright white sparkle envelope. On any
            // currently-washing target (Tildagon, PixMob, StickC LCD)
            // the dispatch layer adds the pulse on top of the wash;
            // PixMob routes through TwoColors(white, current_wash_rgb)
            // so the bracelet flashes white then returns to wash.
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
// PixMob Bench (Epic 11 B-0.5). Three sequential experiments that drive
// the IR HAL directly (bypassing DAL::render_fx) to settle the
// EEPROM-vs-RAM storage question and the cancel-while-illuminated path.
//
// Btn2 cycles tests T1 -> T2 -> T3 -> T1. Btn1 advances within a test.
// step_index_ tracks the within-test step (0/1) so Btn1 toggles between
// "set colour" and "verify behaviour" prompts. Bench operator reads the
// bracelet's behaviour and writes it into epic-11's progress log.
// -------------------------------------------------------------------------

namespace {

// Sized to match PixMobIRDriver's working buffer - any 9-byte PixMob
// command produces under 80 pulses on the wire.
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
        // Cycle through the tests; reset step on each switch. Also stops
        // any in-flight T5 auto-refresh so leaving T5 doesn't keep the IR
        // firing.
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

    // Per-test action. Each test fires a specific IR command and may
    // advance pmob_step_ to track which prompt the LCD shows next.
    switch (pmob_test_) {
        case PMobTest::Test1Persistence:
            // T1 has one IR step: set background purple. Operator pulls
            // and replaces the batteries and observes whether the colour
            // persists. Re-pressing Btn1 just re-fires (harmless).
            pmob_fire_set_background(200, 0, 200);
            pmob_step_ = 1;
            break;
        case PMobTest::Test2Sleep:
            if (pmob_step_ == 0) {
                // Step 0: set background purple, then operator waits for
                // the bracelet to auto-sleep visibly.
                pmob_fire_set_background(200, 0, 200);
                pmob_step_ = 1;
            } else {
                // Step 1: wake the bracelet with a no-op SingleColor at
                // RGB=0 / ASR all-zero. If the background state survived
                // sleep, the bracelet wakes back to purple. If state was
                // cleared, the bracelet wakes to default (or stays dark).
                // After observing, Btn1 again resets to step 0.
                pmob_fire_single_color(
                    0, 0, 0,
                    pixmob::T_0_MS, pixmob::T_0_MS, pixmob::T_0_MS);
                pmob_step_ = 0;
            }
            break;
        case PMobTest::Test3Cancel:
            if (pmob_step_ == 0) {
                // Step 0: set background red and display.
                pmob_fire_set_background(255, 0, 0);
                pmob_step_ = 1;
            } else {
                // Step 1: cancel via set-background-black-and-display.
                // Operator observes: instant black, fades, or no response.
                pmob_fire_set_background(0, 0, 0);
                pmob_step_ = 0;
            }
            break;
        case PMobTest::Test4EnvelopeSweep: {
            // Fire SingleColor(255,255,255) at the current sustain bucket.
            // Attack and release at T_0_MS so the visible duration is
            // mainly the sustain - the operator times what they see and
            // compares to the documented bucket centre.
            const uint8_t sustain_bucket = pmob_step_ & 0x07;
            pmob_fire_single_color(
                255, 255, 255,
                pixmob::T_0_MS, sustain_bucket, pixmob::T_0_MS);
            // Advance to next bucket; wraps after T_3840_MS (index 7).
            pmob_step_ = (pmob_step_ + 1) & 0x07;
            break;
        }
        case PMobTest::Test5AutoRefresh:
            // Cycle through the interval table: OFF -> 3000 -> 2500 -> 2000
            // -> 1500 -> 1000 -> OFF. While interval > 0, tick_pixmob_bench()
            // auto-fires SingleColor(255,255,255, T_0_MS, T_3840_MS,
            // T_0_MS) at the current interval. Square envelope - snap on,
            // hold, snap off - so the visible window is the sustain only
            // (no release fade) and refresh overlap is clean: the new
            // command pre-empts the old envelope and continues the held
            // colour at full brightness with no decay tail to hide.
            pmob_t5_index_ = (pmob_t5_index_ + 1) % kPMobT5IntervalCount;
            if (pmob_t5_index_ != 0) {
                // Fire the first refresh immediately so the operator
                // doesn't wait the interval before seeing anything. The
                // tick fires subsequent refreshes on cadence from here.
                pmob_fire_single_color(
                    255, 255, 255,
                    pixmob::T_0_MS, pixmob::T_3840_MS, pixmob::T_0_MS);
                pmob_t5_last_refresh_ms_ = millis();
            }
            break;
        case PMobTest::Test6TwoColors: {
            // Fire buildTwoColors(red, blue) directly via the IR
            // driver, bypassing any wash state. High contrast colour
            // pair so the brief red flash + held blue tail are both
            // distinguishable. Diagnoses whether the bracelet renders
            // TwoColors visibly in isolation - a precondition for
            // sparkle-on-wash (which encodes via TwoColors) being
            // visible during shows.
            uint16_t buf[kPMobPulseBufSize];
            const size_t n = pixmob::buildTwoColors(
                buf, kPMobPulseBufSize,
                255, 0, 0,     // colour 1: red sparkle
                0, 0, 255);    // colour 2: blue tail (~384 ms hold)
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
    // Square envelope (T_0 / T_3840 / T_0) - no decay tail. New refresh
    // pre-empts the previous sustain cleanly; the bracelet stays at
    // full brightness as long as refresh cadence < sustain duration.
    pmob_fire_single_color(
        255, 255, 255,
        pixmob::T_0_MS, pixmob::T_3840_MS, pixmob::T_0_MS);
    pmob_t5_last_refresh_ms_ = now;
    // Don't update pmob_last_fire_ms_ - the auto-fire is silent on the
    // LCD (operator is watching the bracelet, not the screen). A
    // "Sent!" flash every cycle would just be a distraction.
}

void TestMode::draw_pixmob_bench() {
    using dal::DisplayClearEvent;
    using dal::DisplayShowTextEvent;
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    // Header.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "PMob Bench", WHITE, BLACK, 2});

    // Test label.
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

    // Per-test instruction (kept short for the 160-px-wide LCD).
    // T4 builds the strings dynamically from the current sustain bucket
    // so the user knows what's ABOUT to fire; line1 = bucket name,
    // line2 = nominal ms.
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
            // Documented bucket centres from the pixmob::Time enum.
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
                // Continuation in line2 alone is enough; LCD can't fit much more.
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

    // "Sent!" flash on recent fire.
    if (pmob_last_fire_ms_ != 0
     && (millis() - pmob_last_fire_ms_) < kPMobConfirmFlashMs) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 95, "Sent!", GREEN, BLACK, 2});
    }

    // Footer hint.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: next test  B-hold: back",
        WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
