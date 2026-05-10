// OutputBinding - abstract base for slave-side output bindings.
//
// Concrete bindings subclass this and override the hooks they care
// about. The framework owns one instance per registered binding and
// activates the per-slave-config subset simultaneously: a slave with
// PixMob IR enabled has both LocalDisplayBinding and PixMobIrBinding
// active, so the same incoming RgbPulseEvent fires both surfaces.
// Block 6 lands the contract only; Block 9 migrates the existing
// hardcoded display + PixMob IR forward in slave_mode.cpp into
// concrete LocalDisplayBinding + PixMobIrBinding.
//
// Lifetime:
//   - enter(ctx) / exit(ctx) bracket the active period.
//   - on_light_command fires for every render event the slave decodes
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
#include "dal/dal.h"
#include "hal/input_action.h"

namespace nocturnation {
namespace output_bindings {

class OutputBindingContext;   // forward declaration

class OutputBinding : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override { return plugins::PluginKind::OutputBinding; }

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
