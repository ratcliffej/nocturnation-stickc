#include "local_driver.h"
#include "hal/hal.h"
#include "pixmob_protocol.h"

#include <cstdlib>       // std::rand() for chance roll

#ifdef ARDUINO
#include <Arduino.h>     // now_ms()
#endif

namespace nocturnation {
namespace dal {

namespace {
LocalDriver s_instance;

// now_ms() shim. Native test envs that link this TU (test_dal_*) don't
// pull in modes/ where mode_machine.cpp defines its native millis() seam,
// so we can't depend on millis() being available. Returning 0 in
// native builds means the pulse animation doesn't advance under tests -
// which is fine; those tests exercise registration / dispatch, not
// animation timing.
inline uint32_t now_ms() {
#ifdef ARDUINO
    return ::millis();
#else
    return 0;
#endif
}

// pixmob::Time enum index -> milliseconds. The enum values 0..7 map to a
// fixed table per the protocol; see include/pixmob_protocol.h.
constexpr uint16_t kPixMobTimeMs[8] = {
    0, 32, 96, 192, 480, 960, 2400, 3840
};

// pixmob::Chance enum index -> percentage. Used by roll_chance(); each
// "light" (bracelet OR local screen, per the slave-as-target-device
// model) rolls independently against this percentage so a CHANCE_50
// sparkle fires on roughly half the lights in range.
constexpr uint8_t kPixMobChancePct[8] = {
    100, 88, 67, 50, 32, 16, 10, 4
};

inline uint16_t pixmob_time_ms(pixmob::Time t) {
    return kPixMobTimeMs[static_cast<uint8_t>(t) & 0x07];
}

inline uint8_t pixmob_chance_pct(pixmob::Chance c) {
    return kPixMobChancePct[static_cast<uint8_t>(c) & 0x07];
}

bool roll_chance(pixmob::Chance c) {
    const uint8_t pct = pixmob_chance_pct(c);
    if (pct >= 100) return true;
    return (std::rand() % 100) < pct;
}

inline uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11)
                               | ((g >> 2) << 5)
                               |  (b >> 3));
}
}  // namespace

LocalDriver* local_driver_instance() { return &s_instance; }

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool LocalDriver::begin() {
    bool any_capability_wired = false;

    // Output: display. If the HAL has a real Display, this driver can
    // service display dispatches.
    if (hal::HAL::display() != nullptr) {
        any_capability_wired = true;
    }

    // Input: buttons. Register a HAL callback that bridges button events
    // up to DAL subscribers via deliver_button_press("local", ...).
    if (auto* buttons = hal::HAL::buttons()) {
        buttons->set_callback([](hal::ButtonId id, hal::ButtonEvent kind) {
            ButtonPressEvent ev{id, kind};
            DAL::deliver_button_press("local", ev);
        });
        any_capability_wired = true;
    }

    // Input: mic. Register a HAL frame callback that translates each raw
    // hal::AudioFrame into the DAL's AudioFrameEvent + SpectrumFrameEvent
    // and delivers them to any orchestration-level subscribers. The
    // analyser produces both surfaces in one FFT pass; the DAL splits
    // them across two events so consumers can subscribe to just the
    // band summaries (cheap, most effects) or to the rich spectrum
    // (Epic 4.6 Diagnostic UI, Epic 4.7 modulators) without paying
    // for what they don't need.
    //
    // The mic is NOT started here - orchestration controls lifecycle
    // via DAL::start_audio_input / stop_audio_input, which dispatches
    // into start_audio_input() below.
    if (auto* mic = hal::HAL::mic()) {
        mic->set_frame_callback([](const hal::AudioFrame& frame) {
            // Band-summary event: 3-band B/M/T + 8-band perceptual.
            AudioFrameEvent af;
            af.timestamp_ms  = frame.timestamp_ms;
            af.bass_energy   = frame.bass_energy;
            af.mid_energy    = frame.mid_energy;
            af.treble_energy = frame.treble_energy;
            af.mud           = frame.mud;
            af.sub_bass      = frame.sub_bass;
            af.bass          = frame.bass;
            af.low_mids      = frame.low_mids;
            af.midrange      = frame.midrange;
            af.high_mids     = frame.high_mids;
            af.presence      = frame.presence;
            af.air           = frame.air;
            af.overall_rms   = frame.overall_rms;
            DAL::deliver_audio_frame("local", af);

            // Spectrum-frame event: 32 log-spaced magnitudes. Master-
            // local; Epic 4.7 wires effects-side consumers.
            SpectrumFrameEvent sf;
            sf.timestamp_ms = frame.timestamp_ms;
            for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
                sf.magnitudes[i] = frame.spectrum[i];
            }
            DAL::deliver_spectrum_frame("local", sf);
        });
        any_capability_wired = true;
    }

    // Refuse registration if no capability is wired - there's nothing
    // useful for this driver to do, and registering would just add
    // lookup overhead.
    return any_capability_wired;
}

// -----------------------------------------------------------------------------
// RgbPulse: paint screen full-bleed with attack/sustain/release fade.
//
// The first call kicks off a new animation; subsequent calls during an
// in-flight pulse replace the current state (fresh beat takes over).
// loop_tick() drives the per-frame render at ~30 Hz.
// -----------------------------------------------------------------------------

bool LocalDriver::send(uint8_t /*group_id*/, const RgbPulseEvent& ev) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    if (!pulse_enabled_) return false;

    // Chance roll: each "light" (bracelet OR local screen) rolls
    // independently per the slave-as-target-device model. CHANCE_50 on
    // a sparkle effect paints the screen on roughly half the firings,
    // matching what bracelets in range are doing - the screen behaves
    // like one of many lights.
    if (!roll_chance(ev.chance)) return false;

    pulse_start_ms_   = now_ms();
    attack_ms_        = pixmob_time_ms(ev.attack);
    sustain_ms_       = pixmob_time_ms(ev.sustain);
    release_ms_       = pixmob_time_ms(ev.release);
    target_r_         = ev.r;
    target_g_         = ev.g;
    target_b_         = ev.b;
    pulse_active_     = (attack_ms_ + sustain_ms_ + release_ms_) > 0
                        && (target_r_ | target_g_ | target_b_) != 0;
    pulse_terminated_ = false;

    if (!pulse_active_) {
        // Zero-envelope or all-black: terminate cleanly. We DO paint
        // black into the pulse rect here so a stale colour from a
        // previous pulse doesn't linger.
        d->fill_rect(pulse_rect_x_, pulse_rect_y_,
                     pulse_rect_w_, pulse_rect_h_, 0x0000);
        pulse_terminated_ = true;
        return true;
    }

    // No initial paint - loop_tick on the next main-loop iteration sees
    // pulse_active_=true and paints the right brightness for elapsed=0
    // (full colour for attack=0 envelopes like Rainbow's, otherwise a
    // proper attack ramp). Painting BLACK here would force every fast-
    // attack effect through a one-frame black flicker before the first
    // colour render, which made Rainbow look broken on hardware.
    last_render_ms_ = (pulse_start_ms_ < kFramePeriodMs)
                      ? 0
                      : pulse_start_ms_ - kFramePeriodMs;
    return true;
}

void LocalDriver::loop_tick() {
    if (!pulse_active_) return;
    auto* d = hal::HAL::display();
    if (!d) {
        pulse_active_ = false;
        return;
    }

    const uint32_t now      = now_ms();
    const uint32_t elapsed  = now - pulse_start_ms_;
    const uint32_t total_ms = static_cast<uint32_t>(attack_ms_)
                            + sustain_ms_ + release_ms_;

    // End of envelope: paint final black frame, mark inactive. We always
    // do this draw even if the throttle window hasn't elapsed - it's the
    // pulse's clean termination.
    if (elapsed >= total_ms) {
        if (!pulse_terminated_) {
            d->fill_rect(pulse_rect_x_, pulse_rect_y_,
                         pulse_rect_w_, pulse_rect_h_, 0x0000);
            pulse_terminated_ = true;
        }
        pulse_active_ = false;
        return;
    }

    // Throttle intermediate frames so we don't overrun the SPI bus.
    if (now - last_render_ms_ < kFramePeriodMs) return;
    last_render_ms_ = now;

    // Brightness curve. Linear ramp on RGB888 inputs - the human visual
    // response is non-linear but bracelet hardware uses linear PWM
    // anyway, so this matches what bracelets in the same group are doing.
    float brightness;
    if (elapsed < attack_ms_ && attack_ms_ > 0) {
        brightness = static_cast<float>(elapsed) /
                     static_cast<float>(attack_ms_);
    } else if (elapsed < static_cast<uint32_t>(attack_ms_) + sustain_ms_) {
        brightness = 1.0f;
    } else {
        const uint32_t rel_elapsed = elapsed
                                   - attack_ms_
                                   - sustain_ms_;
        brightness = (release_ms_ > 0)
                   ? 1.0f - (static_cast<float>(rel_elapsed) /
                              static_cast<float>(release_ms_))
                   : 0.0f;
    }
    if (brightness < 0.0f) brightness = 0.0f;
    if (brightness > 1.0f) brightness = 1.0f;

    const uint8_t r = static_cast<uint8_t>(static_cast<float>(target_r_) * brightness);
    const uint8_t g = static_cast<uint8_t>(static_cast<float>(target_g_) * brightness);
    const uint8_t b = static_cast<uint8_t>(static_cast<float>(target_b_) * brightness);
    d->fill_rect(pulse_rect_x_, pulse_rect_y_,
                 pulse_rect_w_, pulse_rect_h_,
                 rgb888_to_rgb565(r, g, b));
}

// -----------------------------------------------------------------------------
// Output dispatchers - delegate to hal::Display
// -----------------------------------------------------------------------------

bool LocalDriver::send(uint8_t /*group_id*/, const DisplayShowTextEvent& ev) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    d->set_text_color(ev.fg_color, ev.bg_color);
    d->set_text_size(ev.size);
    d->draw_text(ev.x, ev.y, ev.text);
    return true;
}

bool LocalDriver::send(uint8_t /*group_id*/, const DisplayClearEvent& ev) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    d->clear(ev.color);
    return true;
}

bool LocalDriver::send(uint8_t /*group_id*/, const DisplayFillRectEvent& ev) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    d->fill_rect(ev.x, ev.y, ev.w, ev.h, ev.color);
    return true;
}

// -----------------------------------------------------------------------------
// Audio input lifecycle
// -----------------------------------------------------------------------------

bool LocalDriver::start_audio_input(uint16_t sample_rate_hz, uint16_t fft_size) {
    auto* mic = hal::HAL::mic();
    if (!mic) return false;
    mic->begin(sample_rate_hz, fft_size);
    return mic->is_running();
}

bool LocalDriver::stop_audio_input() {
    auto* mic = hal::HAL::mic();
    if (!mic) return false;
    mic->end();
    return true;
}

// -----------------------------------------------------------------------------
// Synchronous queries
// -----------------------------------------------------------------------------

int LocalDriver::battery_level() {
    auto* batt = hal::HAL::battery();
    if (!batt) return -1;
    return batt->level_percent();
}

bool LocalDriver::begin_buffered_paint(int x, int y, int w, int h) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    return d->begin_buffered_paint(x, y, w, h);
}

void LocalDriver::end_buffered_paint() {
    auto* d = hal::HAL::display();
    if (!d) return;
    d->end_buffered_paint();
}

bool LocalDriver::send(uint8_t /*group_id*/, const DisplayMeterEvent& ev) {
    auto* d = hal::HAL::display();
    if (!d) return false;
    // Frame
    d->draw_rect(ev.x, ev.y, ev.w, ev.h, ev.frame_color);
    // Bar (clamped to the frame's interior)
    int interior = ev.w - 2;
    if (interior < 0) interior = 0;
    int bar_w = (int)(ev.ratio * (float)interior);
    if (bar_w < 0)        bar_w = 0;
    if (bar_w > interior) bar_w = interior;
    if (bar_w > 0) {
        d->fill_rect(ev.x + 1, ev.y + 1, bar_w, ev.h - 2, ev.bar_color);
    }
    // Optional vertical threshold marker
    if (ev.threshold_ratio >= 0.0f) {
        int thr_x = ev.x + (int)(ev.threshold_ratio * (float)interior);
        if (thr_x < ev.x + ev.w) {
            d->draw_vline(thr_x, ev.y - 2, ev.h + 4, ev.threshold_color);
        }
    }
    return true;
}

}  // namespace dal
}  // namespace nocturnation
