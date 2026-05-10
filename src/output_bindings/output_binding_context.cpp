// OutputBindingContext implementation.
//
// Thin forwarding surface: property bag methods forward to PropertyBag,
// host_caps assembles a mask from the host-surface capabilities the
// HAL declares (the subset bindings care about - Display, IRTx, IRRx,
// ESPNow, Buttons, Battery, Mic). Analyser sub-capabilities are
// intentionally excluded; bindings sit downstream of analysis and have
// no use for them.

#include "output_bindings/output_binding_context.h"
#include "output_bindings/output_binding.h"

#include "hal/hal.h"

#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace output_bindings {

// now_ms() time source. Mirrors the pattern in
// src/visualisations/visualisation_context.cpp and the DAL drivers: on
// Arduino we call ::millis(); on native we use the extern "C" millis()
// symbol that src/modes/mode_machine.cpp provides as a test seam. Envs
// that compile modes/ (e.g. native_modes) get the seam's strong
// definition. The weak fallback below covers envs that link this TU
// without modes/ (e.g. native_output_binding) and returns 0, which is
// fine for tests that pass an explicit now value to mark_entered() and
// only verify the delta arithmetic; native_output_binding tests further
// provide their own strong millis() definition to drive the clock
// explicitly.
}  // namespace output_bindings
}  // namespace nocturnation

#ifndef ARDUINO
extern "C" __attribute__((weak)) uint32_t millis() { return 0; }
#endif

namespace nocturnation {
namespace output_bindings {

namespace {
inline uint32_t now_ms_impl() {
    return millis();
}
}  // namespace

OutputBindingContext::OutputBindingContext(OutputBinding& binding,
                                            plugins::PropertyBag& bag)
    : binding_(&binding), bag_(&bag) {}

plugins::PropertyValue OutputBindingContext::get_property(const char* key) const {
    return bag_->get(key);
}

bool OutputBindingContext::set_property(const char* key,
                                         plugins::PropertyValue value) {
    return bag_->set(key, value);
}

hal::CapabilityMask OutputBindingContext::host_caps() const {
    hal::CapabilityMask m;
    if (hal::HAL::has(hal::Capability::Display)) m.set(hal::Capability::Display);
    if (hal::HAL::has(hal::Capability::IRTx))    m.set(hal::Capability::IRTx);
    if (hal::HAL::has(hal::Capability::IRRx))    m.set(hal::Capability::IRRx);
    if (hal::HAL::has(hal::Capability::ESPNow))  m.set(hal::Capability::ESPNow);
    if (hal::HAL::has(hal::Capability::Buttons)) m.set(hal::Capability::Buttons);
    if (hal::HAL::has(hal::Capability::Battery)) m.set(hal::Capability::Battery);
    if (hal::HAL::has(hal::Capability::Mic))     m.set(hal::Capability::Mic);
    return m;
}

uint32_t OutputBindingContext::now_ms() const {
    return now_ms_impl();
}

uint32_t OutputBindingContext::since_enter_ms() const {
    return now_ms_impl() - entered_at_ms_;
}

void OutputBindingContext::mark_entered(uint32_t now_ms) {
    entered_at_ms_ = now_ms;
}

}  // namespace output_bindings
}  // namespace nocturnation
