// PixMobIrBinding - implementation.
//
// The slave-side PixMob protocol IR transmitter. Operates as a relay
// (is_relay() == true): LumeMode threads the inbound LIGHT_COMMAND
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

#include <cstdio>

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

namespace {

// Built-in target names "all-pixmobs" + "group-1".."group-31". Buffer
// is a thread-unsafe static (caller copies before next call); the IR
// path is single-threaded inside LumeMode::fan_out_light_command so
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

void PixMobIrBinding::on_light_command(OutputBindingContext& ctx,
                                        const RgbPulseEvent& ev) {
    // Relay: pass the inbound NocturNation target_group through as the
    // PixMob protocol group code on the outgoing IR frame. Inbound 0
    // maps to PixMob group 0 ("broadcast to all bracelets in range").
    const uint8_t g = ctx.current_target_group();
    DAL::render_fx(ir_target_name(g), ev);
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
