// AutonomousMasterMode implementation.

#include "autonomous_master_mode.h"

#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/espnow_broadcast_driver.h"
#include "effects/effects.h"
#include "transport/espnow/frame.h"

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
using autonomous_master_detail::Colour;

namespace {

constexpr float         kBaselineAlpha     = 0.02f;
constexpr float         kBeatMultiplier    = 2.5f;
constexpr float         kFluxFloor         = 2000.0f;
constexpr uint32_t      kBeatRefractoryMs  = 200;
constexpr float         kVolumeGate        = 500.0f;
constexpr size_t        kIbiBufferSize     = 8;

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

AutonomousMasterMode::AutonomousMasterMode() : pulse_("all-pixmobs") {}

void AutonomousMasterMode::enter() {
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
    // operator override. The DAL's EspNowBroadcastDriver owns the
    // radio lifecycle now; this mode just toggles it on enter / off
    // on exit. Retransmit pumping + skip-if-recent heartbeat run from
    // the driver's loop_tick (called by DAL::loop_tick every iteration).
    esp_now_broadcast_driver_instance()->start_broadcast(
        persistence::load_master_channel());
    draw();
}

void AutonomousMasterMode::exit() {
    DAL::stop_audio_input("local");
    esp_now_broadcast_driver_instance()->stop_broadcast();
    pulse_.exit();
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

    // Broadcast macro-level musical events (DROP, BREAKDOWN) as
    // MUSIC_EVENT (0x06) frames. Independent of the beat path -
    // a drop can fire on any frame, beat or not, and at most once
    // per genuine transition (the analyser's DropDetector arms
    // and disarms internally). Skipped during pause so the entire
    // deployment stays silent on a single mute press.
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

    // Broadcast LIGHT_COMMAND to any slaves over ESP-NOW. Carries
    // the exact RGB + envelope the master's local IR fire used, so
    // a slave can render the same colour with the same envelope on
    // its screen and be a literal "extra light" in the show.
    //
    // BEAT_DETECTED is intentionally NOT broadcast. The wire format
    // still defines it (transport::espnow::MessageType::BeatDetected
    // = 0x01) for forward compatibility, but no current slave
    // consumes it - all show-rendering on the slave side runs off
    // LIGHT_COMMAND, which is enough. Sending BEAT_DETECTED too
    // doubled the per-beat airtime (each frame burns ~3 retransmits
    // per the Block 5 redundancy strategy) for no visible benefit.
    // If a future slave needs BPM metadata for, say, a numeric
    // readout, the right shape is a textual / scalar message type
    // distinct from the show-effect path - not BEAT_DETECTED.
    //
    // send is async at the radio layer so this returns quickly; the
    // transmission overlaps with the local screen flash + IR fire
    // below. Skipped when paused so the entire deployment goes
    // silent on a single mute press; the periodic heartbeat keeps
    // slaves' master-loss detection from tripping during a pause.
    if (!paused_) {
        uint8_t r=0, g=0, b=0;
        colour_to_rgb(colour_, r, g, b);
        const effects::PulseEnvelope env = effects::envelope_for_bpm(estimated_bpm_);
        RgbPulseEvent wire{};
        wire.r = r; wire.g = g; wire.b = b;
        wire.attack  = env.attack;
        wire.sustain = env.sustain;
        wire.release = env.release;
        wire.chance  = pixmob::CHANCE_100;
        DAL::render_fx("esp-now-broadcast", wire);
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

void AutonomousMasterMode::on_button_event(const ButtonPressEvent& ev) {
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

void AutonomousMasterMode::delay_ms(uint32_t ms) {
#ifdef ARDUINO
    ::delay(ms);
#else
    (void)ms;
#endif
}

void AutonomousMasterMode::sync_pulse_colour() {
    uint8_t r=0, g=0, b=0;
    colour_to_rgb(colour_, r, g, b);
    pulse_.set_colour(r, g, b);
}

void AutonomousMasterMode::update_bpm_from_buffer() {
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

void AutonomousMasterMode::draw() {
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

}  // namespace modes
}  // namespace nocturnation
