// VisualisationContext implementation.
//
// Thin forwarding surface: render_fx forwards to DAL, property bag
// methods forward to PropertyBag, analyser_caps assembles a mask from
// the four analyser sub-capabilities the host's HAL declares.

#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation.h"

#include "hal/hal.h"

#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace visualisations {

// now_ms() time source. Mirrors the pattern in src/dal/drivers/local_driver.cpp
// and src/dal/drivers/espnow_broadcast_driver.cpp: on Arduino we call
// ::millis(); on native we use the extern "C" millis() symbol that
// src/modes/mode_machine.cpp provides as a test seam. Envs that compile
// modes/ (e.g. native_modes) get the seam's strong definition. The
// weak fallback below covers envs that link this TU without modes/
// (e.g. native_visualisation) and returns 0, which is fine for tests
// that pass an explicit now value to mark_entered() and only verify
// the delta arithmetic; native_visualisation tests further provide
// their own strong millis() definition to drive the clock explicitly.
}  // namespace visualisations
}  // namespace nocturnation

#ifndef ARDUINO
extern "C" __attribute__((weak)) uint32_t millis() { return 0; }
#endif

namespace nocturnation {
namespace visualisations {

namespace {
inline uint32_t now_ms_impl() {
    return millis();
}
}  // namespace

VisualisationContext::VisualisationContext(Visualisation& vis,
                                            plugins::PropertyBag& bag)
    : vis_(&vis), bag_(&bag) {}

bool VisualisationContext::render_fx(const char* target,
                                      const dal::RgbPulseEvent& ev) {
    return dal::DAL::render_fx(target, ev);
}

plugins::PropertyValue VisualisationContext::get_property(const char* key) const {
    return bag_->get(key);
}

bool VisualisationContext::set_property(const char* key,
                                         plugins::PropertyValue value) {
    return bag_->set(key, value);
}

hal::CapabilityMask VisualisationContext::analyser_caps() const {
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

uint32_t VisualisationContext::now_ms() const {
    return now_ms_impl();
}

uint32_t VisualisationContext::since_enter_ms() const {
    return now_ms_impl() - entered_at_ms_;
}

void VisualisationContext::mark_entered(uint32_t now_ms) {
    entered_at_ms_ = now_ms;
}

}  // namespace visualisations
}  // namespace nocturnation
