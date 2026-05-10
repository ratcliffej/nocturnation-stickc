// Native test: Plugin base infrastructure (Epic 4.6 Block 3).
//
// Covers:
//   - CapabilityMask: empty, set+has, subset_of (positive + negative),
//     make_capability_mask helper.
//   - PropertyValue: from_X / as_X round-trip for each of the five types.
//   - PropertyBag (native path): defaults, set/get round-trip, clamping
//     of out-of-range numeric/enum, type-mismatch rejection, unknown-key
//     rejection, clear_for_tests reset.
//   - Registry: register / find / at / count, missing/null id handling,
//     capacity overflow, duplicate rejection, clear.
//
// No Arduino NVS in tests - PropertyBag's native build path is in-memory
// only. The Arduino path is exercised at firmware build time only.

#include <unity.h>

#include "hal/capability_mask.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "plugins/registry.h"

using namespace nocturnation;
using nocturnation::hal::Capability;
using nocturnation::hal::CapabilityMask;
using nocturnation::hal::make_capability_mask;
using nocturnation::plugins::Plugin;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Registry;
using nocturnation::plugins::Span;

// -----------------------------------------------------------------------------
// Tiny test plugin. Schema: one bool, one u8 with bounds [10, 200], one
// enum with three values, one colour. id() is "tplug" (5 chars - well
// under the 12-char convention).
// -----------------------------------------------------------------------------

namespace {

const char* const kEnumNames[] = {"low", "med", "high"};

const PropertyDef kSchema[] = {
    {
        /*key=*/"enabled",
        /*type=*/PropertyType::Bool,
        /*default_value=*/PropertyValue::from_bool(true),
        /*min_value=*/PropertyValue::from_bool(false),
        /*max_value=*/PropertyValue::from_bool(true),
        /*display_name=*/"Enabled",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
    {
        /*key=*/"sensitivity",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(128),
        /*min_value=*/PropertyValue::from_u8(10),
        /*max_value=*/PropertyValue::from_u8(200),
        /*display_name=*/"Sensitivity",
        /*unit=*/"%",
        /*enum_names=*/nullptr,
    },
    {
        /*key=*/"intensity",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(1),  // "med"
        /*min_value=*/PropertyValue::from_enum(0),
        /*max_value=*/PropertyValue::from_enum(2),
        /*display_name=*/"Intensity",
        /*unit=*/nullptr,
        /*enum_names=*/kEnumNames,
    },
    {
        /*key=*/"colour",
        /*type=*/PropertyType::Colour,
        /*default_value=*/PropertyValue::from_colour(0x00FF00FFu),  // magenta
        /*min_value=*/PropertyValue{},
        /*max_value=*/PropertyValue{},
        /*display_name=*/"Colour",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
};

class TestPlugin : public Plugin {
public:
    explicit TestPlugin(const char* id, PluginKind kind = PluginKind::Visualisation)
        : id_(id), kind_(kind) {}

    const char* id()           const override { return id_; }
    const char* display_name() const override { return "Test Plugin"; }
    PluginKind  kind()         const override { return kind_; }

    Span<const PropertyDef> properties() const override {
        return Span<const PropertyDef>(kSchema, sizeof(kSchema) / sizeof(kSchema[0]));
    }

    CapabilityMask required_capabilities() const override {
        return make_capability_mask(Capability::Mic);
    }

private:
    const char* id_;
    PluginKind  kind_;
};

// Plugin with no properties + no requirements (for registry tests).
class EmptyPlugin : public Plugin {
public:
    explicit EmptyPlugin(const char* id) : id_(id) {}
    const char* id()           const override { return id_; }
    const char* display_name() const override { return "Empty"; }
    PluginKind  kind()         const override { return PluginKind::Visualisation; }
private:
    const char* id_;
};

}  // namespace

// -----------------------------------------------------------------------------
// Unity setup/teardown
// -----------------------------------------------------------------------------

void setUp(void) {
    PropertyBag::clear_for_tests();
}

void tearDown(void) {}

// -----------------------------------------------------------------------------
// CapabilityMask
// -----------------------------------------------------------------------------

static void test_capability_mask_empty(void) {
    CapabilityMask m;
    TEST_ASSERT_TRUE(m.empty());
    TEST_ASSERT_EQUAL_UINT32(0u, m.raw());
    TEST_ASSERT_FALSE(m.has(Capability::Mic));
    TEST_ASSERT_FALSE(m.has(Capability::Display));
}

static void test_capability_mask_set_has(void) {
    CapabilityMask m;
    m.set(Capability::Mic).set(Capability::IRTx);
    TEST_ASSERT_FALSE(m.empty());
    TEST_ASSERT_TRUE(m.has(Capability::Mic));
    TEST_ASSERT_TRUE(m.has(Capability::IRTx));
    TEST_ASSERT_FALSE(m.has(Capability::Display));
    TEST_ASSERT_FALSE(m.has(Capability::ESPNow));
}

static void test_capability_mask_subset_of(void) {
    CapabilityMask need;
    need.set(Capability::Mic).set(Capability::Display);

    CapabilityMask host_full;
    host_full.set(Capability::Mic)
             .set(Capability::Display)
             .set(Capability::IRTx);
    TEST_ASSERT_TRUE(need.subset_of(host_full));   // every bit in need is in host_full

    CapabilityMask host_short;
    host_short.set(Capability::Mic);  // missing Display
    TEST_ASSERT_FALSE(need.subset_of(host_short));

    // Empty mask is subset of anything.
    CapabilityMask empty;
    TEST_ASSERT_TRUE(empty.subset_of(host_full));
    TEST_ASSERT_TRUE(empty.subset_of(empty));
    // Anything is subset of itself.
    TEST_ASSERT_TRUE(host_full.subset_of(host_full));
}

static void test_capability_mask_make_helper(void) {
    constexpr auto m = make_capability_mask(Capability::Mic,
                                             Capability::IRTx,
                                             Capability::ESPNow);
    TEST_ASSERT_TRUE(m.has(Capability::Mic));
    TEST_ASSERT_TRUE(m.has(Capability::IRTx));
    TEST_ASSERT_TRUE(m.has(Capability::ESPNow));
    TEST_ASSERT_FALSE(m.has(Capability::Display));

    constexpr auto empty_helper = make_capability_mask();
    TEST_ASSERT_TRUE(empty_helper.empty());
}

// -----------------------------------------------------------------------------
// PropertyValue round-trips
// -----------------------------------------------------------------------------

static void test_property_value_bool_round_trip(void) {
    auto t = PropertyValue::from_bool(true);
    TEST_ASSERT_EQUAL(PropertyType::Bool, (int)t.type);
    TEST_ASSERT_TRUE(t.as_bool());
    auto f = PropertyValue::from_bool(false);
    TEST_ASSERT_FALSE(f.as_bool());
}

static void test_property_value_u8_round_trip(void) {
    auto v = PropertyValue::from_u8(42);
    TEST_ASSERT_EQUAL(PropertyType::U8, (int)v.type);
    TEST_ASSERT_EQUAL_UINT8(42, v.as_u8());
    auto edge = PropertyValue::from_u8(255);
    TEST_ASSERT_EQUAL_UINT8(255, edge.as_u8());
}

static void test_property_value_u16_round_trip(void) {
    auto v = PropertyValue::from_u16(1234);
    TEST_ASSERT_EQUAL(PropertyType::U16, (int)v.type);
    TEST_ASSERT_EQUAL_UINT16(1234, v.as_u16());
    auto edge = PropertyValue::from_u16(65535);
    TEST_ASSERT_EQUAL_UINT16(65535, edge.as_u16());
}

static void test_property_value_colour_round_trip(void) {
    auto v = PropertyValue::from_colour(0x00ABCDEFu);
    TEST_ASSERT_EQUAL(PropertyType::Colour, (int)v.type);
    TEST_ASSERT_EQUAL_UINT32(0x00ABCDEFu, v.as_colour());
}

static void test_property_value_enum_round_trip(void) {
    auto v = PropertyValue::from_enum(2);
    TEST_ASSERT_EQUAL(PropertyType::Enum, (int)v.type);
    TEST_ASSERT_EQUAL_UINT8(2, v.as_enum());
}

// -----------------------------------------------------------------------------
// PropertyBag (native path)
// -----------------------------------------------------------------------------

static void test_property_bag_returns_default_when_unset(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    TEST_ASSERT_TRUE(bag.get("enabled").as_bool());                // default true
    TEST_ASSERT_EQUAL_UINT8(128, bag.get("sensitivity").as_u8());  // default 128
    TEST_ASSERT_EQUAL_UINT8(1, bag.get("intensity").as_enum());    // default 1 ("med")
    TEST_ASSERT_EQUAL_UINT32(0x00FF00FFu, bag.get("colour").as_colour());
}

static void test_property_bag_set_then_get(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);

    TEST_ASSERT_TRUE(bag.set("enabled", PropertyValue::from_bool(false)));
    TEST_ASSERT_FALSE(bag.get("enabled").as_bool());

    TEST_ASSERT_TRUE(bag.set("sensitivity", PropertyValue::from_u8(75)));
    TEST_ASSERT_EQUAL_UINT8(75, bag.get("sensitivity").as_u8());

    TEST_ASSERT_TRUE(bag.set("intensity", PropertyValue::from_enum(2)));
    TEST_ASSERT_EQUAL_UINT8(2, bag.get("intensity").as_enum());

    TEST_ASSERT_TRUE(bag.set("colour", PropertyValue::from_colour(0x00112233u)));
    TEST_ASSERT_EQUAL_UINT32(0x00112233u, bag.get("colour").as_colour());
}

static void test_property_bag_clamps_u8_below_min(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    TEST_ASSERT_TRUE(bag.set("sensitivity", PropertyValue::from_u8(5)));  // below min 10
    TEST_ASSERT_EQUAL_UINT8(10, bag.get("sensitivity").as_u8());
}

static void test_property_bag_clamps_u8_above_max(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    TEST_ASSERT_TRUE(bag.set("sensitivity", PropertyValue::from_u8(250)));  // above max 200
    TEST_ASSERT_EQUAL_UINT8(200, bag.get("sensitivity").as_u8());
}

static void test_property_bag_clamps_enum_above_max(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    TEST_ASSERT_TRUE(bag.set("intensity", PropertyValue::from_enum(7)));  // above max 2
    TEST_ASSERT_EQUAL_UINT8(2, bag.get("intensity").as_enum());
}

static void test_property_bag_rejects_type_mismatch(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    // Try to write a U16 to a U8 schema slot.
    TEST_ASSERT_FALSE(bag.set("sensitivity", PropertyValue::from_u16(50)));
    // Default still in place.
    TEST_ASSERT_EQUAL_UINT8(128, bag.get("sensitivity").as_u8());
    // Bool to enum: rejected.
    TEST_ASSERT_FALSE(bag.set("intensity", PropertyValue::from_bool(true)));
    TEST_ASSERT_EQUAL_UINT8(1, bag.get("intensity").as_enum());
}

static void test_property_bag_rejects_unknown_key(void) {
    TestPlugin plugin("tplug");
    PropertyBag bag(plugin);
    TEST_ASSERT_FALSE(bag.set("nonexistent", PropertyValue::from_u8(99)));
    // get() of unknown key returns default-constructed PropertyValue.
    PropertyValue v = bag.get("nonexistent");
    TEST_ASSERT_EQUAL(PropertyType::Bool, (int)v.type);
    TEST_ASSERT_EQUAL_UINT32(0u, v.raw);
}

static void test_property_bag_clear_for_tests_resets_state(void) {
    TestPlugin plugin("tplug");
    {
        PropertyBag bag(plugin);
        TEST_ASSERT_TRUE(bag.set("sensitivity", PropertyValue::from_u8(75)));
        TEST_ASSERT_EQUAL_UINT8(75, bag.get("sensitivity").as_u8());
    }
    PropertyBag::clear_for_tests();
    {
        PropertyBag bag2(plugin);
        TEST_ASSERT_EQUAL_UINT8(128, bag2.get("sensitivity").as_u8());  // back to default
    }
}

static void test_property_bag_namespace_separates_kinds(void) {
    // Two plugins sharing the same id but different kinds get distinct
    // NVS namespaces ("nv_<id>" vs "nb_<id>"), so writes don't collide.
    TestPlugin vis("shared", PluginKind::Visualisation);
    TestPlugin out("shared", PluginKind::OutputBinding);
    PropertyBag vis_bag(vis);
    PropertyBag out_bag(out);
    TEST_ASSERT_TRUE(vis_bag.set("sensitivity", PropertyValue::from_u8(50)));
    TEST_ASSERT_TRUE(out_bag.set("sensitivity", PropertyValue::from_u8(150)));
    TEST_ASSERT_EQUAL_UINT8(50,  vis_bag.get("sensitivity").as_u8());
    TEST_ASSERT_EQUAL_UINT8(150, out_bag.get("sensitivity").as_u8());
}

// -----------------------------------------------------------------------------
// Registry
// -----------------------------------------------------------------------------

static void test_registry_register_and_count(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("a");
    EmptyPlugin b("b");
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_TRUE(reg.register_plugin(&a));
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_TRUE(reg.register_plugin(&b));
    TEST_ASSERT_EQUAL_size_t(2, reg.count());
}

static void test_registry_find_by_id(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("alpha");
    EmptyPlugin b("beta");
    reg.register_plugin(&a);
    reg.register_plugin(&b);
    TEST_ASSERT_EQUAL_PTR(&a, reg.find("alpha"));
    TEST_ASSERT_EQUAL_PTR(&b, reg.find("beta"));
}

static void test_registry_find_missing_returns_null(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("alpha");
    reg.register_plugin(&a);
    TEST_ASSERT_NULL(reg.find("zulu"));
    TEST_ASSERT_NULL(reg.find(""));
    TEST_ASSERT_NULL(reg.find(nullptr));
}

static void test_registry_at_returns_entry_and_oob_null(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("a");
    EmptyPlugin b("b");
    reg.register_plugin(&a);
    reg.register_plugin(&b);
    TEST_ASSERT_EQUAL_PTR(&a, reg.at(0));
    TEST_ASSERT_EQUAL_PTR(&b, reg.at(1));
    TEST_ASSERT_NULL(reg.at(2));
    TEST_ASSERT_NULL(reg.at(99));
}

static void test_registry_capacity_overflow_rejected(void) {
    Registry<Plugin, 2> reg;
    EmptyPlugin a("a");
    EmptyPlugin b("b");
    EmptyPlugin c("c");
    TEST_ASSERT_TRUE(reg.register_plugin(&a));
    TEST_ASSERT_TRUE(reg.register_plugin(&b));
    TEST_ASSERT_FALSE(reg.register_plugin(&c));   // capacity 2 reached
    TEST_ASSERT_EQUAL_size_t(2, reg.count());
}

static void test_registry_duplicate_rejected(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("a");
    TEST_ASSERT_TRUE(reg.register_plugin(&a));
    TEST_ASSERT_FALSE(reg.register_plugin(&a));   // same pointer
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
}

static void test_registry_null_register_rejected(void) {
    Registry<Plugin, 4> reg;
    TEST_ASSERT_FALSE(reg.register_plugin(nullptr));
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
}

static void test_registry_clear(void) {
    Registry<Plugin, 4> reg;
    EmptyPlugin a("a");
    EmptyPlugin b("b");
    reg.register_plugin(&a);
    reg.register_plugin(&b);
    TEST_ASSERT_EQUAL_size_t(2, reg.count());
    reg.clear();
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_NULL(reg.find("a"));
}

// -----------------------------------------------------------------------------
// Plugin defaults
// -----------------------------------------------------------------------------

static void test_plugin_default_required_capabilities_empty(void) {
    EmptyPlugin p("e");
    TEST_ASSERT_TRUE(p.required_capabilities().empty());
}

static void test_plugin_default_properties_empty(void) {
    EmptyPlugin p("e");
    TEST_ASSERT_EQUAL_size_t(0, p.properties().size);
}

static void test_plugin_default_power_profile(void) {
    EmptyPlugin p("e");
    auto profile = p.power();
    TEST_ASSERT_TRUE(profile.needs_audio_frames);
    TEST_ASSERT_FALSE(profile.needs_spectrum_frame);
    TEST_ASSERT_FALSE(profile.needs_8band_summary);
    TEST_ASSERT_EQUAL_UINT16(20, profile.lcd_refresh_hz_max);
    TEST_ASSERT_EQUAL_UINT16(0,  profile.tick_hz);
}

// -----------------------------------------------------------------------------
// main
// -----------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_capability_mask_empty);
    RUN_TEST(test_capability_mask_set_has);
    RUN_TEST(test_capability_mask_subset_of);
    RUN_TEST(test_capability_mask_make_helper);

    RUN_TEST(test_property_value_bool_round_trip);
    RUN_TEST(test_property_value_u8_round_trip);
    RUN_TEST(test_property_value_u16_round_trip);
    RUN_TEST(test_property_value_colour_round_trip);
    RUN_TEST(test_property_value_enum_round_trip);

    RUN_TEST(test_property_bag_returns_default_when_unset);
    RUN_TEST(test_property_bag_set_then_get);
    RUN_TEST(test_property_bag_clamps_u8_below_min);
    RUN_TEST(test_property_bag_clamps_u8_above_max);
    RUN_TEST(test_property_bag_clamps_enum_above_max);
    RUN_TEST(test_property_bag_rejects_type_mismatch);
    RUN_TEST(test_property_bag_rejects_unknown_key);
    RUN_TEST(test_property_bag_clear_for_tests_resets_state);
    RUN_TEST(test_property_bag_namespace_separates_kinds);

    RUN_TEST(test_registry_register_and_count);
    RUN_TEST(test_registry_find_by_id);
    RUN_TEST(test_registry_find_missing_returns_null);
    RUN_TEST(test_registry_at_returns_entry_and_oob_null);
    RUN_TEST(test_registry_capacity_overflow_rejected);
    RUN_TEST(test_registry_duplicate_rejected);
    RUN_TEST(test_registry_null_register_rejected);
    RUN_TEST(test_registry_clear);

    RUN_TEST(test_plugin_default_required_capabilities_empty);
    RUN_TEST(test_plugin_default_properties_empty);
    RUN_TEST(test_plugin_default_power_profile);

    return UNITY_END();
}
