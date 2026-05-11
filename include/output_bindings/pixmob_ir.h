// PixMobIrBinding - slave-side IR forward to PixMob bracelets.
//
// Migrated from SlaveMode::render_light's `render_fx(ir_target, ev)` path
// in Epic 4.6 Block 9. The pre-migration SlaveMode held a `slave_ir_group_`
// uint8_t loaded from NVS key `slv_ir_grp` and mapped it to a target
// device name ("all-pixmobs" if 0, "group-N" if 1..5) which was then
// passed to DAL::render_fx. This binding owns that responsibility now:
// the "group" property sits in its property bag (NVS namespace
// "nb_pixmob-ir"), enter() resolves it, on_light_command maps it to
// the same target name and fires DAL::render_fx(target, ev).
//
// Wire output is byte-identical to the pre-migration path. The
// PixMob byte-parity tests are the regression net.
//
// Properties:
//   "group" : Enum, 0..5, default 0 (All / Group 1 / Group 2 / Group 3 /
//             Group 4 / Group 5).
//
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

    // PixMobIrBinding is a relay binding: it transmits PixMob protocol
    // IR with the inbound target_group as the PixMob group code, so the
    // slave's own slv_group filter is bypassed for this binding. PixMob
    // bracelets do their own group filtering at the IR protocol level.
    bool is_relay() const override { return true; }

    hal::CapabilityMask                       required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties()            const override;

    void on_light_command(OutputBindingContext&,
                           const dal::RgbPulseEvent&) override;
};

// Singletons. main.cpp registers via:
//   output_binding_registry().register_plugin(pixmob_ir_instance());
PixMobIrBinding*      pixmob_ir_instance();
plugins::PropertyBag& pixmob_ir_property_bag();
OutputBindingContext& pixmob_ir_context();

}  // namespace output_bindings
}  // namespace nocturnation
