// PixMobIrBinding - implementation.
//
// The Lume-side PixMob protocol IR transmitter. Operates as a relay
// (is_relay() == true): LumeMode threads the inbound LIGHT_PULSE
// target_group through OutputBindingContext::current_target_group();
// this binding passes it straight through as the PixMob protocol group
// code on the outgoing IR frame. PixMob bracelets do their own group
// filtering at the protocol level.
//
// Inbound target_group = 0 maps to PixMob protocol group 0 ("broadcast
// to every bracelet in IR range"). The pre-Epic-4.65 per-binding
// configured group property was removed because it was effectively dead
// once the relay path landed - the inbound NocturNation target_group is
// the right axis for which PixMob bracelets respond, and the broadcast
// fallback "broadcast to all" is the only sensible default.

#include "output_bindings/pixmob_ir.h"
#include "output_bindings/output_binding_context.h"

#include "dal/dal.h"
#include "dal/drivers/pixmob_ir_driver.h"
#include "transport/espnow/frame.h"

#include <cstdio>

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

namespace {

// Built-in target names "all-pixmobs" + "group-1".."group-31". Buffer
// is a thread-unsafe static (caller copies before next call); the IR
// path is single-threaded inside LumeMode::fan_out_light_pulse so
// this is safe in practice. 12-char max ("group-31\0") well under
// the 16-byte buffer.
const char* ir_target_name(uint8_t group_id) {
    if (group_id == 0) return "all-pixmobs";
    if (group_id > 31) return "all-pixmobs";
    static char buf[16];
    std::snprintf(buf, sizeof(buf), "group-%u", static_cast<unsigned>(group_id));
    return buf;
}

}  // namespace

// =============================================================================
// PixMobIrBinding
// =============================================================================

hal::CapabilityMask PixMobIrBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::IRTx);
}

void PixMobIrBinding::on_light_pulse(OutputBindingContext& ctx,
                                        const RgbPulseEvent& ev) {
    // Relay: pass the inbound NocturNation target_group through as the
    // PixMob protocol group code on the outgoing IR frame. Inbound 0
    // maps to PixMob group 0 ("broadcast to all bracelets in range").
    const uint8_t g = ctx.current_target_group();
    DAL::render_fx(ir_target_name(g), ev);
}

void PixMobIrBinding::on_light_wash(
    OutputBindingContext& ctx,
    const transport::espnow::LightWashPayload& p) {
    // Relay wash to bracelets near this Lume. The IR driver singleton
    // holds the per-group wash state (started_ms, anchors, cycle_ms,
    // refresh_interval_ms) and fires the periodic SingleColor refresh
    // from its own loop_tick - the binding just hands off the cue and
    // walks away, matching the Director-side loopback path in
    // DAL::render_wash. IR-only (no ESP-NOW re-broadcast) so a Lume
    // relaying its inbound wash doesn't echo back to the Director.
    auto* drv = pixmob_ir_driver_instance();
    if (!drv || !drv->enabled()) return;
    const uint8_t g = ctx.current_target_group();
    if (g >= PixMobIRDriver::kWashSlots) return;

    LightWashEvent ev{};
    ev.r1 = p.r1; ev.g1 = p.g1; ev.b1 = p.b1;
    ev.r2 = p.r2; ev.g2 = p.g2; ev.b2 = p.b2;
    ev.attack         = p.attack;
    ev.release        = p.release;
    ev.intensity      = p.intensity;
    ev.cycle_ms       = p.cycle_ms;
    ev.ttl_seconds    = p.ttl_seconds;
    ev.pulse_response = p.pulse_response;
    drv->send_wash(g, ev);
    drv->increment_send_count();
}

void PixMobIrBinding::on_light_wash_end(OutputBindingContext& ctx,
                                           uint8_t release_time,
                                           uint8_t led_mode,
                                           uint8_t /*led_modifier1*/,
                                           uint8_t /*led_modifier2*/) {
    // PixMob is a single-pixel Lume - it doesn't declare
    // Capability::AddressableLeds. LedMode!=0 is not addressed at PixMob
    // bracelets; drop silently to avoid ending an ongoing wash the
    // Director didn't intend to cancel on us.
    if (led_mode != 0) return;
    auto* drv = pixmob_ir_driver_instance();
    if (!drv || !drv->enabled()) return;
    const uint8_t g = ctx.current_target_group();
    if (g >= PixMobIRDriver::kWashSlots) return;
    drv->send_wash_end(g, release_time);
    drv->increment_send_count();
}

void PixMobIrBinding::on_light_wash_pulse(OutputBindingContext& ctx,
                                             const RgbPulseEvent& ev) {
    // LIGHT_WASH_PULSE routes to the IR driver's send_wash_pulse,
    // which composes the sparkle as TwoColors(sparkle, current_wash)
    // when a wash is active on this group, or falls back to a plain
    // SingleColor pulse when there isn't (matches the spec: a
    // wash-pulse on a non-washing target degrades gracefully to a
    // normal pulse).
    auto* drv = pixmob_ir_driver_instance();
    if (!drv || !drv->enabled()) return;
    const uint8_t g = ctx.current_target_group();
    drv->send_wash_pulse(g, ev);
    drv->increment_send_count();
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
PixMobIrBinding      s_instance;
PropertyBag          s_bag(s_instance);     // empty - binding has no properties
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

PixMobIrBinding*      pixmob_ir_instance() { return &s_instance; }
OutputBindingContext& pixmob_ir_context()  { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
