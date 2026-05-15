// OutputBindingContext - services surface exposed to active output bindings.
//
// The framework owns one context per active binding and threads it
// into every hook on OutputBinding. Bindings reach hardware via this
// context; they never include hal/hal.h or dal/dal.h directly so
// future hosts (Tildagon, ...) can be swapped in without touching
// binding code.
//
// Asymmetry with VisualisationContext: this does NOT expose
// render_fx() - bindings ARE the render destination, so calling
// render_fx from inside a binding would be circular. Bindings perform
// their hardware action via the HAL surfaces the framework wires
// through (concrete bindings in Block 9 reach hal::HAL::display() /
// hal::HAL::ir_tx() directly; this context exposes the property bag,
// capability query, and time helpers shared by every binding).

#pragma once

#include <cstdint>

#include "plugins/property_bag.h"
#include "hal/capability_mask.h"

namespace nocturnation {
namespace output_bindings {

class OutputBinding;

class OutputBindingContext {
public:
    OutputBindingContext(OutputBinding& binding, plugins::PropertyBag& bag);

    // -- Property bag -----------------------------------------------------
    plugins::PropertyValue get_property(const char* key) const;
    bool                   set_property(const char* key, plugins::PropertyValue value);

    // -- Capability query -------------------------------------------------
    // CapabilityMask of host capabilities a binding might care about
    // (Display, IRTx, IRRx, ESPNow, Buttons, Battery, Mic). Bindings
    // query this in enter() to verify the surfaces they need are still
    // available (defensive: required_capabilities() is also checked at
    // registration / activation time). Analyser sub-capabilities are
    // intentionally NOT included - those are visualisation territory;
    // bindings sit downstream of analysis.
    hal::CapabilityMask host_caps() const;

    // -- Time helpers -----------------------------------------------------
    // now_ms uses the same source as the framework's loop_tick (millis()
    // on Arduino, the test seam in mode_machine on native). The
    // visualisation_context.cpp / mode_machine.cpp pattern is reused: a
    // weak millis() fallback returning 0, with the strong definition
    // provided by mode_machine.cpp on envs that link it, or by the test
    // TU directly on standalone-binding test envs.
    uint32_t now_ms()         const;
    uint32_t since_enter_ms() const;

    void     mark_entered(uint32_t now_ms);

    // -- Inbound-frame addressing (Epic 4.65 Block 5) ---------------------
    // LumeMode sets these on every LIGHT_COMMAND fan-out before calling
    // the binding's on_light_command. Relay bindings (e.g. PixMobIrBinding)
    // read current_target_group() to thread the inbound group code into
    // their downstream protocol. Local bindings can ignore both - the
    // slv_group filter has already gated their fan-out at the Lume level.
    // Default 0 when no LIGHT_COMMAND is in flight (broadcast semantics).
    uint8_t current_target_class() const { return current_target_class_; }
    uint8_t current_target_group() const { return current_target_group_; }
    void    set_current_target(uint8_t target_class, uint8_t target_group) {
        current_target_class_ = target_class;
        current_target_group_ = target_group;
    }

    // -- Identity ---------------------------------------------------------
    OutputBinding&        binding()      const { return *binding_; }
    plugins::PropertyBag& property_bag() const { return *bag_; }

private:
    OutputBinding*        binding_;
    plugins::PropertyBag* bag_;
    uint32_t              entered_at_ms_ = 0;
    uint8_t               current_target_class_ = 0;
    uint8_t               current_target_group_ = 0;
};

}  // namespace output_bindings
}  // namespace nocturnation
