// PixMobIrBinding - implementation (Epic 4.6 Block 9).
//
// Extracted from src/modes/slave_mode.cpp's render_light() second
// render_fx call. The group-id-to-target-name mapping is the same
// switch statement that lived in SlaveMode::ir_target_name, just keyed
// off the property bag "group" value instead of the in-memory
// slave_ir_group_ field. The DAL::render_fx call is unchanged.
//
// Wire output is byte-identical to the pre-migration code path.

#include "output_bindings/pixmob_ir.h"
#include "output_bindings/output_binding_context.h"

#include "dal/dal.h"

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;

// =============================================================================
// Property schema
// =============================================================================

namespace {

const char* const kGroupNames[] = {
    "All", "Group 1", "Group 2", "Group 3", "Group 4", "Group 5"
};

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"group",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(0),  // All
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(5),
        /*display_name=*/"Slave Group",
        /*unit=*/nullptr,
        /*enum_names=*/kGroupNames,
    },
};

constexpr size_t kPropCount = sizeof(kProps) / sizeof(kProps[0]);

// Map group id (0..5) -> registered DAL device name. Group 0 maps to
// "all-pixmobs" for full-broadcast behaviour; 1..5 map to the per-group
// devices DAL::begin() registers. Mirrors the pre-migration
// SlaveMode::ir_target_name switch byte-for-byte.
const char* ir_target_name(uint8_t group_id) {
    switch (group_id) {
        case 0:  return "all-pixmobs";
        case 1:  return "group-1";
        case 2:  return "group-2";
        case 3:  return "group-3";
        case 4:  return "group-4";
        case 5:  return "group-5";
        default: return "all-pixmobs";
    }
}

}  // namespace

// =============================================================================
// PixMobIrBinding
// =============================================================================

hal::CapabilityMask PixMobIrBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::IRTx);
}

Span<const PropertyDef> PixMobIrBinding::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

void PixMobIrBinding::on_light_command(OutputBindingContext& ctx,
                                        const RgbPulseEvent& ev) {
    // Epic 4.65 Block 5: relay-binding semantics. The inbound LIGHT_COMMAND
    // carries a target_group byte (the NocturNation addressing group);
    // SlaveMode threads it into ctx.current_target_group() before
    // calling here. PixMob bracelets do their own filtering at the IR
    // protocol level on the group byte, so we pass the inbound group
    // straight through as the PixMob protocol group code.
    //
    // When the inbound target_group is 0 (broadcast across all groups),
    // we fall back to the binding's configured "group" property - the
    // operator's preferred default PixMob group for this slave. That
    // preserves the pre-Epic-4.65 behaviour of "broadcast LIGHT_COMMAND
    // -> fire PixMob IR with my locally-configured group" for installations
    // that haven't migrated to class:group addressing.
    uint8_t g = ctx.current_target_group();
    if (g == 0) {
        g = ctx.get_property("group").as_enum();
    }
    DAL::render_fx(ir_target_name(g), ev);
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
PixMobIrBinding      s_instance;
PropertyBag          s_bag(s_instance);
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

PixMobIrBinding*      pixmob_ir_instance()     { return &s_instance; }
PropertyBag&          pixmob_ir_property_bag() { return s_bag; }
OutputBindingContext& pixmob_ir_context()      { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
