// OutputBinding - abstract base for Lume-side output bindings.
//
// Concrete bindings subclass this and override the hooks they care
// about. The framework owns one instance per registered binding and
// activates the per-Lume-config subset simultaneously: a Lume with
// PixMob IR enabled has both LocalDisplayBinding and PixMobIrBinding
// active, so the same incoming RgbPulseEvent fires both surfaces.
// Block 6 lands the contract only; Block 9 migrates the existing
// hardcoded display + PixMob IR forward in lume_mode.cpp into
// concrete LocalDisplayBinding + PixMobIrBinding.
//
// Lifetime:
//   - enter(ctx) / exit(ctx) bracket the active period.
//   - on_light_command fires for every render event the Lume decodes
//     from incoming ESP-NOW LIGHT_COMMAND frames while this binding
//     is active. This is the primary hook - bindings turn the colour
//     event into a hardware action (paint LCD, transmit PixMob IR
//     pulse, forward over DMX, drive a Tildagon LED ring, ...).
//   - on_input_action fires when the user produces an InputEvent and
//     a binding has opted into receiving them (e.g. a diagnostic
//     test-fire button). Default no-op; bindings are not user-facing
//     by default.
//   - tick(ctx, now_ms) fires from the framework's loop_tick at the
//     cadence declared by power().tick_hz (0 = never; binding is
//     purely event-driven). Useful for time-based effects like a
//     fade-out after a pulse.
//
// Asymmetry with Visualisation: a binding's "render" surface is direct
// hardware access via HAL, not a render_fx forward (calling render_fx
// from inside a binding would be circular - bindings ARE the render
// destination). The BindingContext exposes the HAL pointers and
// services a binding needs.

#pragma once

#include <cstdint>

#include "plugins/plugin.h"
#include "hal/device_class.h"
#include "dal/dal.h"
#include "hal/input_action.h"

namespace nocturnation {
namespace output_bindings {

class OutputBindingContext;   // forward declaration

class OutputBinding : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override { return plugins::PluginKind::OutputBinding; }

    // Device-class taxonomy (Epic 4.65). Director encodes the chosen class
    // into LIGHT_COMMAND.target_class; LumeMode filters inbound frames
    // against this value per active binding. Pure-virtual so every
    // binding declares its class explicitly - silent defaults would let
    // bindings drift untagged and break the addressing contract. Never
    // returns DeviceClass::All (that value is the addressing wildcard,
    // not a device identity).
    virtual hal::DeviceClass device_class() const = 0;

    // Relay flag (Epic 4.65 Block 5). When true the binding is a
    // pass-through to a downstream protocol that does its own group
    // filtering (PixMob bracelets check the IR group code at their
    // own level), so the Lume's slv_group filter is BYPASSED for this
    // binding - it fires whenever the target_class matches, regardless
    // of whether target_group matches the Lume's own group. The
    // binding's on_light_command reads OutputBindingContext::
    // current_target_group() to relay the inbound group code into the
    // downstream protocol. Default: false (local binding; slv_group
    // filter applies). PixMobIrBinding overrides to true.
    virtual bool is_relay() const { return false; }

    virtual void enter(OutputBindingContext&) {}
    virtual void exit (OutputBindingContext&) {}

    // Primary hook: render the incoming colour event on this binding's
    // output surface.
    virtual void on_light_command(OutputBindingContext&,
                                   const dal::RgbPulseEvent&) {}

    // Optional: bindings that need to react to user input (e.g. a
    // diagnostic test-fire button). Default no-op.
    virtual void on_input_action(OutputBindingContext&,
                                  const hal::InputEvent&) {}

    // Optional: time-driven binding work (e.g. fade-out after a pulse).
    // Default no-op; framework calls only if power().tick_hz > 0.
    virtual void tick(OutputBindingContext&, uint32_t now_ms) {}
};

}  // namespace output_bindings
}  // namespace nocturnation
