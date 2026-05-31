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

#include <cstdint>

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

    // Epic 6C Phase F: WASH-family hooks. on_light_wash captures the
    // params + stamps wash_start_ms; tick() drives the attack ramp,
    // cosine-eased ping-pong drift, TTL, and release fade. Pulses that
    // arrive during an active wash are rendered as additive overlay
    // (or silently dropped when pulse_response == 0).
    void on_light_wash      (OutputBindingContext&,
                              const transport::espnow::LightWashPayload&) override;
    void on_light_wash_end  (OutputBindingContext&, uint8_t release_time) override;
    void on_light_wash_pulse(OutputBindingContext&,
                              const dal::RgbPulseEvent&) override;
    void tick(OutputBindingContext&, uint32_t now_ms) override;

private:
    enum class WashPhase : uint8_t { Inactive, Attacking, Holding, Releasing };

    // Wash state machine.
    WashPhase  wash_phase_           = WashPhase::Inactive;
    uint32_t   wash_started_ms_      = 0;       // when current wash began (for cosine phase)
    uint32_t   wash_phase_started_ms_= 0;       // when current phase began (for fades)
    uint8_t    wash_r1_              = 0,  wash_g1_ = 0,  wash_b1_ = 0;
    uint8_t    wash_r2_              = 0,  wash_g2_ = 0,  wash_b2_ = 0;
    uint8_t    wash_attack_units_    = 0;       // 100 ms units
    uint8_t    wash_release_units_   = 0;       // 100 ms units (default; LIGHT_WASH_END may override)
    uint8_t    wash_intensity_       = 255;
    uint16_t   wash_cycle_ms_        = 0;
    uint16_t   wash_ttl_seconds_     = 0;
    uint8_t    wash_pulse_response_  = 0;
    // The colour the binding was rendering before this wash started -
    // used as the attack-lerp source. Defaults to black on a cold start.
    uint8_t    pre_wash_r_           = 0, pre_wash_g_ = 0, pre_wash_b_ = 0;
    // The release fade end-state. For LIGHT_WASH_END this is always black
    // (per design); kept here so the same fade-out code path serves both
    // TTL-expiry and explicit cancel.
    uint8_t    release_units_active_ = 0;       // active release duration, in 100 ms units
    uint8_t    release_end_r_        = 0, release_end_g_ = 0, release_end_b_ = 0;

    // Pulse overlay state. The pulse contribution is additively blended
    // onto the live wash baseline each tick, then fades over its release
    // window. Only meaningful while a wash is active; the existing
    // non-wash on_light_pulse path falls through to render_fx as before.
    bool       pulse_active_         = false;
    uint32_t   pulse_started_ms_     = 0;
    uint16_t   pulse_total_ms_       = 0;       // attack + sustain + release in ms
    uint8_t    pulse_r_              = 0, pulse_g_ = 0, pulse_b_ = 0;
};

// Singletons. main.cpp registers via:
//   output_binding_registry().register_plugin(local_display_instance());
LocalDisplayBinding*  local_display_instance();
plugins::PropertyBag& local_display_property_bag();
OutputBindingContext& local_display_context();

}  // namespace output_bindings
}  // namespace nocturnation
