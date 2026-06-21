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
//   - on_light_pulse fires for every render event the Lume decodes
//     from incoming ESP-NOW LIGHT_PULSE frames while this binding
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
#include "transport/espnow/frame.h"   // LightWashPayload for on_light_wash hook

namespace nocturnation {
namespace output_bindings {

class OutputBindingContext;   // forward declaration

// Per-binding capability surface (Epic 6C Phase B). Each OutputBinding
// declares what it can physically do with light. The Lume-side dispatch
// uses these flags to silently drop messages a binding cannot act on
// (e.g. WASH-family frames on PixMob-class bindings that cannot hold
// state). Extensible: future flags (can_strobe, can_text, etc.) slot in
// without breaking existing bindings - they default-init to false.
//
// Capability vocabulary:
//   can_pulse   - fire an ASR pulse (universal; every binding sets true).
//   can_wash    - hold a persistent background wash (LIGHT_WASH family).
//   can_overlay - render a PULSE additively on top of an active WASH.
//
// See docs/lume-capabilities-design.md for the full contract.
struct BindingCapabilities {
    bool can_pulse;
    bool can_wash;
    bool can_overlay;
};

class OutputBinding : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override { return plugins::PluginKind::OutputBinding; }

    // Device-class taxonomy (Epic 4.65). Director encodes the chosen class
    // into LIGHT_PULSE.target_class; LumeMode filters inbound frames
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
    // binding's on_light_pulse reads OutputBindingContext::
    // current_target_group() to relay the inbound group code into the
    // downstream protocol. Default: false (local binding; slv_group
    // filter applies). PixMobIrBinding overrides to true.
    virtual bool is_relay() const { return false; }

    virtual void enter(OutputBindingContext&) {}
    virtual void exit (OutputBindingContext&) {}

    // Primary hook: render the incoming colour event on this binding's
    // output surface.
    virtual void on_light_pulse(OutputBindingContext&,
                                   const dal::RgbPulseEvent&) {}

    // WASH-family hooks (Epic 6C Phase F). The Lume-side dispatch
    // capability-gates these on `can_wash = true` - bindings that
    // declare can_wash = false never receive them, so default no-op
    // is harmless for pulse-only bindings. See
    // lume-capabilities-design.md for the semantic contract.
    //
    //   on_light_wash       - enter or refresh wash state. The binding
    //                          stamps its own wash_start_ms on receipt
    //                          for the cosine-eased drift phase. A
    //                          second call to the same target supersedes.
    //   on_light_wash_end   - cancel the active wash, fading to black
    //                          over release_time (100 ms units).
    //   on_light_wash_pulse - fire as additive overlay on the live wash
    //                          baseline. Drops silently if no wash active.
    virtual void on_light_wash(OutputBindingContext&,
                                const transport::espnow::LightWashPayload&) {}
    virtual void on_light_wash_end(OutputBindingContext&,
                                    uint8_t /*release_time*/) {}
    virtual void on_light_wash_pulse(OutputBindingContext&,
                                      const dal::RgbPulseEvent&) {}

    // Display family hooks (Epic 13). Dispatched only on bindings whose
    // device_class() is DeviceClass::Display (orthogonal to the wash
    // family, which targets Light / MultiLedScreen). Bindings that don't
    // care leave the defaults as no-ops. See [[epic-13-lume-display-content]]
    // for the per-payload semantic contract.
    //
    //   on_text_display - render a header/body string set onto the bound
    //                      surface, optionally TTL-bounded.
    //   on_bitmap_header - prepare to receive a multi-plane bitmap; stage
    //                       dimensions + colours + checksum. Render only
    //                       after all planes arrive and the checksum matches.
    //   on_bitmap_plane  - chunk of one plane's pixel bytes. Appends into
    //                       the staging buffer at byte_offset.
    //   on_clear_screen  - clear the text and/or bitmap layer on this surface.
    virtual void on_text_display(OutputBindingContext&,
                                  const transport::espnow::TextDisplayPayload&) {}
    virtual void on_bitmap_header(OutputBindingContext&,
                                   const transport::espnow::BitmapHeaderPayload&) {}
    virtual void on_bitmap_plane(OutputBindingContext&,
                                  const transport::espnow::BitmapPlanePayload&) {}
    virtual void on_clear_screen(OutputBindingContext&,
                                  const transport::espnow::ClearScreenPayload&) {}

    // Optional: bindings that need to react to user input (e.g. a
    // diagnostic test-fire button). Default no-op.
    virtual void on_input_action(OutputBindingContext&,
                                  const hal::InputEvent&) {}

    // Optional: time-driven binding work (e.g. fade-out after a pulse).
    // Default no-op; framework calls only if power().tick_hz > 0.
    virtual void tick(OutputBindingContext&, uint32_t now_ms) {}

    // Capability surface (Epic 6C Phase B). Pure-virtual so every
    // binding declares its physical capabilities explicitly - silent
    // defaults would let a new binding inherit "{true, true, true}"
    // by accident, which the dispatch layer then interprets as "the
    // binding can hold a wash" and a wash will be silently lost.
    // See BindingCapabilities above for the vocabulary.
    virtual BindingCapabilities capabilities() const = 0;
};

}  // namespace output_bindings
}  // namespace nocturnation
