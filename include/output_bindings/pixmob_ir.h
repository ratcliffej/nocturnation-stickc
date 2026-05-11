// PixMobIrBinding - slave-side PixMob protocol IR transmitter.
//
// Migrated from SlaveMode::render_light in Epic 4.6 Block 9, then
// refactored to a pure relay binding in Epic 4.65 Block 5 / follow-up.
// SlaveMode threads the inbound LIGHT_COMMAND target_group through
// OutputBindingContext::current_target_group(); this binding passes it
// straight through as the PixMob protocol group code on the outgoing
// IR frame. PixMob bracelets do their own group filtering at the
// protocol level (bracelet's stored group ID checked against the IR
// frame's group byte; 0 in the frame = "all bracelets respond").
//
// The pre-Epic-4.65 per-binding "group" property is gone - it was a
// pre-relay artifact whose only remaining use was a broadcast fallback,
// and "broadcast to all PixMobs" (group 0) is the only sensible default.
// The slave's own NocturNation receive-filter group lives at
// persistence::load_slv_group() (Config > Group) and is applied by
// SlaveMode upstream of the binding fan-out.
//
// No properties.
// Power profile: defaults (event-driven; no audio/spectrum/tick needs).
// Capability requirements: IRTx.

#pragma once

#include "output_bindings/output_binding.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace output_bindings {

class PixMobIrBinding : public OutputBinding {
public:
    const char* id()           const override { return "pixmob-ir"; }
    const char* display_name() const override { return "PixMob IR"; }

    hal::DeviceClass device_class() const override {
        return hal::DeviceClass::Light;
    }

    // Relay binding: transmits PixMob protocol IR with the inbound
    // target_group as the PixMob group code, so the slave's slv_group
    // filter is bypassed for this binding. PixMob bracelets do their
    // own group filtering at the IR protocol level.
    bool is_relay() const override { return true; }

    hal::CapabilityMask required_capabilities() const override;

    void on_light_command(OutputBindingContext&,
                           const dal::RgbPulseEvent&) override;
};

// Singletons. main.cpp registers via:
//   output_binding_registry().register_plugin(pixmob_ir_instance());
PixMobIrBinding*      pixmob_ir_instance();
OutputBindingContext& pixmob_ir_context();

}  // namespace output_bindings
}  // namespace nocturnation
