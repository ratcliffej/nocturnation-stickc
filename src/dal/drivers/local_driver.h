// LocalDriver - the DAL driver for the "local" transport.
//
// Bridges the host's own HAL capabilities into the DAL's typed event
// interface. On output, translates display events into hal::Display calls.
// On input, registers a HAL Buttons callback that delivers ButtonPressEvent
// up through DAL::deliver_button_press("local", ...).
//
// Registered automatically by DAL::begin() when at least one local
// capability is wired up (i.e. hal::HAL::display() or hal::HAL::buttons()
// returns a non-null instance). On hosts without those backends, the
// LocalDriver does not register and fire_display_*("local", ...) calls
// return false silently - which is the correct fail-silent behaviour.

#pragma once

#include "dal/analyser/beat_detector.h"
#include "dal/analyser/drop_detector.h"
#include "dal/analyser/music_descriptors.h"
#include "dal/dal.h"

namespace nocturnation {
namespace dal {

class LocalDriver : public Driver {
public:
    const char* transport_name() const override { return "local"; }

    bool begin() override;
    void loop_tick() override;     // drives RgbPulse ASR animation on screen

    bool send(uint8_t group_id, const RgbPulseEvent&)         override;
    bool send(uint8_t group_id, const DisplayShowTextEvent&)  override;
    bool send(uint8_t group_id, const DisplayClearEvent&)     override;
    bool send(uint8_t group_id, const DisplayFillRectEvent&)  override;
    bool send(uint8_t group_id, const DisplayMeterEvent&)     override;

    bool start_audio_input(uint16_t sample_rate_hz, uint16_t fft_size) override;
    bool stop_audio_input() override;

    int  battery_level() override;

    // True while an RgbPulseEvent is mid-render. Orchestration that owns
    // the same screen (SlaveMode showing status text, etc.) should use
    // this to defer its own draws until the pulse finishes - otherwise
    // the status redraw cuts the fade short.
    bool is_pulse_active() const { return pulse_active_; }

    // Stop any in-flight pulse animation immediately - no further loop_tick
    // paints will happen. Use when orchestration is about to take over the
    // screen for a non-pulse draw (e.g. exiting Test mode back to the
    // menu) and doesn't want the next animation frame to overdraw the new
    // content. Does NOT paint anything itself; the caller is responsible
    // for the next screen state.
    void cancel_pulse() {
        pulse_active_     = false;
        pulse_terminated_ = true;
    }

    // Pass-through to hal::Display's buffered paint session API. Wrap a
    // multi-element draw (e.g. SlaveMode's status strip - icons + text)
    // in begin/end and the backend batches all the intermediate
    // fill_rect / draw_text calls into one SPI burst, eliminating the
    // tearing that comes from the panel scanning out mid-burst between
    // many small writes.
    bool begin_buffered_paint(int x, int y, int w, int h);
    void end_buffered_paint();

    // Configure the screen rectangle that pulses paint into. Default is
    // full-screen; orchestration that wants to keep a persistent overlay
    // (e.g. Slave mode's battery + signal-strength icon strip at the top)
    // shrinks this rectangle so its overlay area stays untouched by pulse
    // frames. Coordinates are display-space, in pixels.
    void set_pulse_rect(int x, int y, int w, int h) {
        pulse_rect_x_ = x;
        pulse_rect_y_ = y;
        pulse_rect_w_ = w;
        pulse_rect_h_ = h;
    }
    void reset_pulse_rect() {
        // Default: cover the whole 240x135 panel both Stick reference
        // platforms expose. Future hosts with different screens can call
        // set_pulse_rect with their own dimensions.
        set_pulse_rect(0, 0, 240, 135);
    }

    // Per-capability gate for RgbPulse on the local screen. When false,
    // render_fx("local", RgbPulseEvent{...}) returns false silently
    // without painting; other LocalDriver outputs (DisplayShowText,
    // DisplayClear, etc.) are unaffected. Toggled via Config > Display
    // > Pulse Enable, persisted to NVS as "scr_puls_en".
    void set_pulse_enabled(bool e)         { pulse_enabled_ = e; }
    bool pulse_enabled() const             { return pulse_enabled_; }

private:
    // -------------------------------------------------------------------------
    // RgbPulse animation state. The screen IS this host's primary "light",
    // so an inbound RgbPulseEvent (whether from local effect code or
    // forwarded from an ESP-NOW LIGHT_COMMAND on a slave) becomes a timed
    // attack/sustain/release fade rather than an instant paint.
    //
    // Throttled to ~30 Hz: any faster and the SPI display can't keep up;
    // any slower and short envelope phases (T_32_MS = 32 ms) wouldn't
    // get more than one frame.
    // -------------------------------------------------------------------------

    bool      pulse_active_     = false;
    bool      pulse_terminated_ = true;   // last frame already drawn as black
    bool      pulse_enabled_    = true;   // Config > Display > Pulse Enable

    // Default: full-screen rectangle. Slave mode shrinks this so the top
    // status strip is preserved across pulse paints.
    int       pulse_rect_x_     = 0;
    int       pulse_rect_y_     = 0;
    int       pulse_rect_w_     = 240;
    int       pulse_rect_h_     = 135;

    uint32_t  pulse_start_ms_   = 0;
    uint16_t  attack_ms_        = 0;
    uint16_t  sustain_ms_       = 0;
    uint16_t  release_ms_       = 0;
    uint8_t   target_r_         = 0;
    uint8_t   target_g_         = 0;
    uint8_t   target_b_         = 0;

    uint32_t  last_render_ms_   = 0;
    static constexpr uint32_t kFramePeriodMs = 33;   // ~30 Hz cap

    // Sub-band adaptive-threshold onset detectors. All three run on
    // every FFT cycle over the same SpectrumFrame; results stamp onto
    // AudioFrameEvent fields so orchestration consumers don't have to
    // re-run threshold logic. The "BeatDetector" name is legacy from
    // Epic 4.5; the class itself is a generic per-band onset detector
    // that Block 3 instantiates three times with different watched-band
    // ranges and tuning constants:
    //   beat_detector_  - kick band  (~30-150 Hz, k=2.2, refractory 200 ms)
    //   snare_detector_ - snare band (~200-2000 Hz, k=2.0, refractory 150 ms)
    //   hihat_detector_ - hi-hat band (~4-8 kHz, k=1.8, refractory 80 ms)
    // Configs for the three Block 3 detectors. Returned by static
    // helpers so the in-class member initialisers below stay short.
    // Band ranges derived from the canonical 16 kHz / 512 FFT
    // operating point: ratio_per_band ~= 1.191 covering [30, 8000) Hz
    // in 32 log-spaced bands. Block 6 hardware tuning may refine.
    static analyser::BeatDetectorConfig snare_config() {
        analyser::BeatDetectorConfig c;
        c.watch_start  = 11;     // ~200 Hz
        c.watch_count  = 13;     // through band 23, ~2 kHz
        c.threshold_k  = 2.0f;   // slightly lower than kick - snare
                                 // transients are smaller relative
                                 // to baseline mid-band energy
        c.refractory_ms = 150;   // shorter than kick - snare-fill
                                 // patterns can fire faster
        return c;
    }
    static analyser::BeatDetectorConfig hihat_config() {
        analyser::BeatDetectorConfig c;
        c.watch_start  = 27;     // ~3.5 kHz
        c.watch_count  = 5;      // through band 31, ~8 kHz Nyquist
        c.threshold_k  = 1.8f;   // smaller transients again
        c.refractory_ms = 80;    // 16th-note hi-hat patterns can
                                 // hit ~12 Hz; 80 ms allows up to
                                 // 12.5 Hz before merging hits
        return c;
    }

    analyser::BeatDetector beat_detector_;
    analyser::BeatDetector snare_detector_{snare_config()};
    analyser::BeatDetector hihat_detector_{hihat_config()};

    // Drop and breakdown detector. Same lifecycle as beat_detector_ -
    // one update per FFT cycle, the result is stamped onto each
    // AudioFrameEvent's music_event field. Master orchestration then
    // broadcasts MUSIC_EVENT frames over ESP-NOW when the field is
    // non-zero.
    analyser::DropDetector drop_detector_;

    // Continuous music descriptors (Epic 4.7 Block 3). Computes
    // centroid + energy + density per frame and stamps the u8 values
    // onto AudioFrameEvent.{centroid, energy, density}.
    analyser::MusicDescriptors music_descriptors_;
};

// Singleton accessor used by DAL::begin() to register the driver.
LocalDriver* local_driver_instance();

}  // namespace dal
}  // namespace nocturnation
