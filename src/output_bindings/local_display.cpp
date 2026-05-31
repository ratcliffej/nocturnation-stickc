// LocalDisplayBinding - implementation (Epic 4.6 Block 9).
//
// Extracted from src/modes/lume_mode.cpp's render_light() first
// render_fx call. Just forwards the event to the "local" target via
// DAL::render_fx. Wire output is byte-identical to the pre-migration
// code path - same call, same args.

#include "output_bindings/local_display.h"
#include "output_bindings/output_binding_context.h"

#include "dal/dal.h"

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

// =============================================================================
// LocalDisplayBinding
// =============================================================================

hal::CapabilityMask LocalDisplayBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Display);
}

void LocalDisplayBinding::on_light_pulse(OutputBindingContext& /*ctx*/,
                                            const RgbPulseEvent& ev) {
    // Local light surface (screen on the StickC). Future hosts with
    // both an LCD and onboard LEDs (e.g. Tildagon) get both via the
    // same "local" target - LocalDriver fans out per host capability.
    DAL::render_fx("local", ev);
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
LocalDisplayBinding  s_instance;
PropertyBag          s_bag(s_instance);
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

LocalDisplayBinding*  local_display_instance()     { return &s_instance; }
PropertyBag&          local_display_property_bag() { return s_bag; }
OutputBindingContext& local_display_context()      { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
