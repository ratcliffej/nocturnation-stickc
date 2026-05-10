// AutonomousMasterMode implementation (Epic 4.6 Block 8 onwards).
//
// Thin shell over the active Visualisation. Mode owns: pause flag,
// flux/baseline tracking for the on-screen meter, music_event broadcast,
// status display, lifecycle (audio input + ESP-NOW broadcast). The
// per-beat render fan-out + BPM tracking + Colour enum all moved to
// BeatPulseVisualisation.

#include "autonomous_master_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "transport/espnow/frame.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"
#include "visualisations/beat_pulse.h"
#include "hal/input_action.h"

#include <cstdio>

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
using nocturnation::visualisations::visualisation_registry;
using nocturnation::visualisations::beat_pulse_colour_label;
using nocturnation::visualisations::beat_pulse_colour_screen_rgb;
using nocturnation::visualisations::beat_pulse_estimated_bpm;
using nocturnation::visualisations::beat_pulse_context;

namespace {

constexpr float    kBeatMultiplier = 2.5f;
constexpr float    kBaselineAlpha  = 0.02f;
constexpr float    kVolumeGate     = 500.0f;

}  // namespace

void AutonomousMasterMode::enter() {
    // Reset display-only flux/baseline state.
    baseline_flux_     = 100.0f;
    prev_bass_energy_  = 0.0f;
    current_flux_      = 0.0f;
    current_level_     = 0.0f;
    last_draw_ms_      = 0;
    paused_            = false;

    // Resolve the active vis. Block 8 hardcodes BeatPulse; Block 10 will
    // load the last-used vis ID from NVS via persistence::load_active_vis().
    active_vis_ = visualisation_registry().find("beat-pulse");
    ctx_        = &beat_pulse_context();

    if (ctx_) ctx_->set_paused(paused_);
    if (ctx_) ctx_->mark_entered(millis());
    if (active_vis_ && ctx_) active_vis_->enter(*ctx_);

    DAL::start_audio_input("local", 16000, 512);
    // Channel from NVS. DAL's EspNowBroadcastDriver owns the radio lifecycle
    // now; this mode just toggles it on enter / off on exit. Retransmit
    // pumping + skip-if-recent heartbeat run from the driver's loop_tick.
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_master_channel());
    draw();
}

void AutonomousMasterMode::exit() {
    if (active_vis_ && ctx_) active_vis_->exit(*ctx_);
    DAL::stop_audio_input("local");
    esp_now_broadcast_driver_instance()->stop_broadcast();
}

void AutonomousMasterMode::loop_tick() {
    const uint32_t now = millis();
    if (now - last_draw_ms_ > 50) {
        draw();
        last_draw_ms_ = now;
    }
}

void AutonomousMasterMode::on_audio_frame(const AudioFrameEvent& ev) {
    current_level_ = ev.overall_rms;

    if (current_level_ < kVolumeGate) {
        prev_bass_energy_ = 0.0f;
        return;
    }

    // Track flux + baseline for the on-screen meter only. Beat firing
    // is owned by the DAL analyser's BeatDetector (Epic 4.5 Block 3);
    // these values have no bearing on which frames fire beats.
    float flux = ev.bass_energy - prev_bass_energy_;
    if (flux < 0) flux = 0;
    prev_bass_energy_ = ev.bass_energy;
    current_flux_     = flux;

    baseline_flux_ = baseline_flux_ * (1.0f - kBaselineAlpha)
                   + flux * kBaselineAlpha;

    // Broadcast macro-level musical events (DROP / BREAKDOWN / BUILD)
    // as MUSIC_EVENT (0x06) frames. Independent of the beat path -
    // a drop can fire on any frame, beat or not, and at most once per
    // genuine transition (the analyser's DropDetector arms / disarms
    // internally). Skipped during pause so the entire deployment stays
    // silent on a single mute press. Mode-level concern (not vis).
    if (ev.music_event != 0 && !paused_) {
#ifdef ARDUINO
        const char* name = (ev.music_event == 1) ? "DROP"
                         : (ev.music_event == 2) ? "BREAKDOWN"
                         : (ev.music_event == 3) ? "BUILD"
                         :                         "?";
        Serial.printf("[MUSIC] %s at %lu ms (bass_energy=%.1f)\n",
                      name,
                      static_cast<unsigned long>(millis()),
                      ev.bass_energy);
#endif
        esp_now_broadcast_driver_instance()->send_music_event(
            static_cast<transport::espnow::MusicEventType>(ev.music_event));
    }

    // Forward the frame to the active vis. The vis owns BPM tracking +
    // per-beat render fan-out; pause is mirrored into ctx so the vis
    // can early-return on render without missing BPM tracking.
    if (active_vis_ && ctx_) {
        active_vis_->on_audio_frame(*ctx_, ev);
    }
}

void AutonomousMasterMode::on_button_event(const ButtonPressEvent& ev) {
    // Btn2 (side) short: cycle colour. Synthesise an InputAction::Cycle
    // event and route it through the vis. Block 10 will replace this
    // with a real DAL::subscribe_input_actions subscription; for Block 8
    // the synthesis preserves UX without touching the FSM wiring.
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::Pressed) {
        if (active_vis_ && ctx_) {
            const hal::InputEvent ev_in{hal::InputAction::Cycle, millis()};
            active_vis_->on_input_action(*ctx_, ev_in);
        }
        draw();
        return;
    }
    // Btn1 (front) short: pause / resume toggle. Mirror to ctx so the
    // vis early-returns on render without missing BPM tracking.
    //
    // Block 13 was scheduled to fix a pre-existing bug where pause-toggle
    // fired a test pulse via pulse_.on_beat() on both the way in and the
    // way out. The migration moves the Pulse instance into the vis, so
    // the buggy call site is no longer reachable; the fix lands here a
    // block early. Pause toggle no longer fires a pulse on either edge -
    // the architect-recommended behaviour.
    if (ev.id == ButtonId::Btn1 && ev.kind == ButtonEvent::Pressed) {
        paused_ = !paused_;
        if (ctx_) ctx_->set_paused(paused_);
        return;
    }
    // Btn2 long-press: back to menu. Two-button UI on both Plus2 and S3 -
    // BtnPWR is hardware-managed on the S3 (PMIC owns reset/off), so the
    // firmware UI is consistently BtnA + BtnB across hosts.
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        ModeMachine::switch_to(ModeId::Menu);
        return;
    }
}

void AutonomousMasterMode::draw() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    char title[32];
    std::snprintf(title, sizeof(title), " %s%s",
                  beat_pulse_colour_label(), paused_ ? " : Muted" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, title, WHITE, BLACK, 3});

    char bpm[24];
    const float est = beat_pulse_estimated_bpm();
    if (est > 0.0f) {
        std::snprintf(bpm, sizeof(bpm), " BPM: %.0f", est);
    } else {
        std::snprintf(bpm, sizeof(bpm), " BPM: ---");
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 40, bpm, WHITE, BLACK, 2});

    // Batt + IR fire counter on one line. IR count is <=4 chars with k/M
    // suffix above 10000 so the line stays within 240 px at size 2.
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

}  // namespace modes
}  // namespace nocturnation
