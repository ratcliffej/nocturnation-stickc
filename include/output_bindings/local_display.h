// LocalDisplayBinding - Lume-side local-screen render of inbound
// LIGHT_PULSE events.
//
// Migrated from LumeMode::render_light's first `DAL::render_fx("local",
// ev)` line in Epic 4.6 Block 9. The pre-migration LumeMode fired
// render_fx("local", ev) unconditionally for every decoded
// LIGHT_PULSE payload, which routed through LocalDriver to paint the
// pulse rect. This binding owns that responsibility now: on_light_pulse
// forwards the same event to the same "local" target.
//
// No properties yet. Future blocks may add brightness / pulse-rect
// overrides; the property bag scaffolding is in place via the binding's
// "nb_local-display" NVS namespace whenever those land.
//
// Power profile: defaults (event-driven; no audio/spectrum/tick needs).
// Capability requirements: Display.

#pragma once

#include "output_bindings/output_binding.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace output_bindings {

class LocalDisplayBinding : public OutputBinding {
public:
    const char* id()           const override { return "local-display"; }
    const char* display_name() const override { return "Local Display"; }

    hal::DeviceClass device_class() const override {
        return hal::DeviceClass::Screen;
    }

    hal::CapabilityMask required_capabilities() const override;

    // The Stick's LCD is a screen-as-light surface: it can hold a
    // colour persistently (wash), and a PULSE-overlay on top of an
    // active wash is just a brighter frame for a moment via the same
    // draw path. Full capability set per Epic 6C Phase B / design doc.
    BindingCapabilities capabilities() const override {
        return BindingCapabilities{
            /*can_pulse=*/   true,
            /*can_wash=*/    true,
            /*can_overlay=*/ true,
        };
    }

    void on_light_pulse(OutputBindingContext&,
                           const dal::RgbPulseEvent&) override;
};

// Singletons. main.cpp registers via:
//   output_binding_registry().register_plugin(local_display_instance());
LocalDisplayBinding*  local_display_instance();
plugins::PropertyBag& local_display_property_bag();
OutputBindingContext& local_display_context();

}  // namespace output_bindings
}  // namespace nocturnation
