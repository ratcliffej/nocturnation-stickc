// ShowContext implementation (Epic 4.7).
//
// Thin forwarding surface: render_fx forwards to DAL, property bag
// methods forward to PropertyBag, analyser_caps assembles a mask from
// the four analyser sub-capabilities the host's HAL declares. Mirrors
// visualisation_context.cpp from Epic 4.6 - kept as a parallel TU for
// now because Epic 4.7's Visualisation retirement (Block 2) will drop
// the Vis TU entirely.

#include "shows/show_context.h"
#include "shows/show.h"

#include "hal/hal.h"

#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace shows {
}  // namespace shows
}  // namespace nocturnation

// Native millis() seam. On Arduino we use the framework's ::millis().
// On native test builds modes/ provides a strong definition that tests
// drive via test_seam::set_millis(); the weak fallback here returns 0
// so native envs that link this TU without modes/ still resolve.
#ifndef ARDUINO
extern "C" __attribute__((weak)) uint32_t millis() { return 0; }
#endif

namespace nocturnation {
namespace shows {

namespace {
inline uint32_t now_ms_impl() {
    return millis();
}
}  // namespace

ShowContext::ShowContext(Show& show, plugins::PropertyBag& bag)
    : show_(&show), bag_(&bag) {}

bool ShowContext::render_fx(const char* target,
                             const dal::RgbPulseEvent& ev) {
    return dal::DAL::render_fx(target, ev);
}

bool ShowContext::render_wash(const char* target,
                               const dal::LightWashEvent& ev) {
    return dal::DAL::render_wash(target, ev);
}
bool ShowContext::render_wash_end(const char* target, uint8_t release_time) {
    return dal::DAL::render_wash_end(target, release_time);
}
bool ShowContext::render_wash_pulse(const char* target,
                                     const dal::RgbPulseEvent& ev) {
    return dal::DAL::render_wash_pulse(target, ev);
}

plugins::PropertyValue ShowContext::get_property(const char* key) const {
    return bag_->get(key);
}

bool ShowContext::set_property(const char* key,
                                plugins::PropertyValue value) {
    return bag_->set(key, value);
}

hal::CapabilityMask ShowContext::analyser_caps() const {
    hal::CapabilityMask m;
    if (hal::HAL::has(hal::Capability::AnalyserBeatDetection)) {
        m.set(hal::Capability::AnalyserBeatDetection);
    }
    if (hal::HAL::has(hal::Capability::AnalyserDropDetection)) {
        m.set(hal::Capability::AnalyserDropDetection);
    }
    if (hal::HAL::has(hal::Capability::AnalyserSpectrumFrame)) {
        m.set(hal::Capability::AnalyserSpectrumFrame);
    }
    if (hal::HAL::has(hal::Capability::AnalyserBandSummary)) {
        m.set(hal::Capability::AnalyserBandSummary);
    }
    return m;
}

uint32_t ShowContext::now_ms() const {
    return now_ms_impl();
}

uint32_t ShowContext::since_enter_ms() const {
    return now_ms_impl() - entered_at_ms_;
}

void ShowContext::mark_entered(uint32_t now_ms) {
    entered_at_ms_ = now_ms;
}

}  // namespace shows
}  // namespace nocturnation
