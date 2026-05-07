#include "local_driver.h"
#include "hal/hal.h"

namespace nocturnation {
namespace dal {

namespace {
LocalDriver s_instance;
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
    // hal::AudioFrame into the DAL's AudioFrameEvent and delivers it to
    // any orchestration-level subscribers. The mic is NOT started here -
    // orchestration controls lifecycle via DAL::start_audio_input /
    // stop_audio_input, which dispatches into start_audio_input() below.
    if (auto* mic = hal::HAL::mic()) {
        mic->set_frame_callback([](const hal::AudioFrame& frame) {
            AudioFrameEvent ev;
            ev.timestamp_ms  = frame.timestamp_ms;
            ev.bass_energy   = frame.bass_energy;
            ev.mid_energy    = frame.mid_energy;
            ev.treble_energy = frame.treble_energy;
            ev.overall_rms   = frame.overall_rms;
            DAL::deliver_audio_frame("local", ev);
        });
        any_capability_wired = true;
    }

    // Refuse registration if no capability is wired - there's nothing
    // useful for this driver to do, and registering would just add
    // lookup overhead.
    return any_capability_wired;
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
