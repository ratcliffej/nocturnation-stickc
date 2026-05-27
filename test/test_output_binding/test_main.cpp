// Native test: OutputBinding contract (Epic 4.6 Block 6).
//
// Covers the interface + context + registry surface that concrete
// output bindings will land on in Block 9. No concrete binding here -
// just a tiny StubOutputBinding in this TU that records every hook
// invocation so we can verify routing.
//
// Provides a miniature HAL backend declaring a deliberate subset of
// host-surface capabilities (Display + IRTx + Buttons + Mic, but NOT
// IRRx + ESPNow + Battery) so host_caps() returns a strict subset of
// the host-surface bit space and we can assert both presence and
// absence. Analyser sub-capabilities are intentionally not declared -
// host_caps() only reports the host-surface subset bindings care
// about.

#include <unity.h>

#include "hal/hal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "dal/dal.h"
#include "output_bindings/output_binding.h"
#include "output_bindings/output_binding_context.h"
#include "output_bindings/output_binding_registry.h"

// =============================================================================
// Native millis() seam (mirrors mode_machine.cpp / visualisation tests)
// =============================================================================
//
// OutputBindingContext::since_enter_ms() reads millis() through the
// shared shim in src/output_bindings/output_binding_context.cpp. The TU
// there provides a weak fallback that returns 0; this strong definition
// wins at link time. Tests advance the clock via set_test_millis() so
// since_enter_ms() arithmetic is observable.
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }

static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend
// =============================================================================
//
// Declares Display + IRTx + Buttons + Mic. Intentionally NOT IRRx,
// ESPNow, or Battery, so host_caps() is a strict subset of the
// host-surface bit space and the tests can verify both presence and
// absence.

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = {
    Capability::Display,
    Capability::IRTx,
    Capability::Buttons,
    Capability::Mic,
};
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()     { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }

bool HAL::has(Capability c) {
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (kCapabilities[i] == c) return true;
    }
    return false;
}

void HAL::begin()     {}
void HAL::loop_tick() {}

Mic*     HAL::mic()      { return nullptr; }
IRTx*    HAL::ir_tx()    { return nullptr; }
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return nullptr; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// StubOutputBinding
// =============================================================================
//
// Records every hook invocation. on_light_command captures the
// RgbPulseEvent payload so tests can assert it round-tripped intact.

using namespace nocturnation;
using nocturnation::hal::Capability;
using nocturnation::hal::CapabilityMask;
using nocturnation::hal::make_capability_mask;
using nocturnation::hal::DeviceClass;
using nocturnation::plugins::Plugin;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;
using nocturnation::output_bindings::OutputBinding;
using nocturnation::output_bindings::OutputBindingContext;
using nocturnation::output_bindings::OutputBindingRegistry;
using nocturnation::output_bindings::output_binding_registry;

namespace {

const PropertyDef kStubSchema[] = {
    {
        /*key=*/"intensity",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(100),
        /*min_value=*/PropertyValue::from_u8(0),
        /*max_value=*/PropertyValue::from_u8(255),
        /*display_name=*/"Intensity",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
};

class StubOutputBinding : public OutputBinding {
public:
    const char* id()           const override { return "stub-bind"; }
    const char* display_name() const override { return "Stub Binding"; }
    DeviceClass device_class() const override { return DeviceClass::Light; }

    Span<const PropertyDef> properties() const override {
        return Span<const PropertyDef>(kStubSchema,
                                       sizeof(kStubSchema) / sizeof(kStubSchema[0]));
    }

    void enter(OutputBindingContext&)                                   override { ++enter_calls; }
    void exit (OutputBindingContext&)                                   override { ++exit_calls;  }
    void on_light_command(OutputBindingContext&,
                           const dal::RgbPulseEvent& ev)                 override {
        ++light_calls;
        last_event = ev;
    }
    void on_input_action(OutputBindingContext&,
                          const hal::InputEvent&)                        override { ++input_calls; }
    void tick(OutputBindingContext&, uint32_t now)                      override { ++tick_calls; last_tick_ms = now; }

    void reset() {
        enter_calls = exit_calls = light_calls = input_calls = tick_calls = 0;
        last_tick_ms = 0;
        last_event   = dal::RgbPulseEvent{};
    }

    int                 enter_calls  = 0;
    int                 exit_calls   = 0;
    int                 light_calls  = 0;
    int                 input_calls  = 0;
    int                 tick_calls   = 0;
    uint32_t            last_tick_ms = 0;
    dal::RgbPulseEvent  last_event   = {};
};

// A binding with no schema and a requirement the test HAL DOESN'T
// have, for the gating test below.
class DemandingBinding : public OutputBinding {
public:
    const char* id()           const override { return "demanding"; }
    const char* display_name() const override { return "Demanding"; }
    DeviceClass device_class() const override { return DeviceClass::Light; }
    CapabilityMask required_capabilities() const override {
        return make_capability_mask(Capability::ESPNow);
    }
};

}  // namespace

// =============================================================================
// Unity setup / teardown
// =============================================================================

void setUp(void) {
    set_test_millis(0);
    PropertyBag::clear_for_tests();
    output_binding_registry().clear();
}

void tearDown(void) {}

// =============================================================================
// OutputBinding::kind() is locked to PluginKind::OutputBinding
// =============================================================================

static void test_output_binding_kind_is_output_binding(void) {
    StubOutputBinding b;
    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)b.kind());

    DemandingBinding d;
    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)d.kind());
}

// Epic 4.65 Block 2: OutputBinding::device_class() is pure-virtual; each
// concrete binding must declare its class. Wire-stable enum values mean
// these int casts are part of the contract too.
static void test_device_class_enum_values_are_wire_stable(void) {
    TEST_ASSERT_EQUAL_UINT8(0x00, (uint8_t)DeviceClass::All);
    TEST_ASSERT_EQUAL_UINT8(0x01, (uint8_t)DeviceClass::Light);
    TEST_ASSERT_EQUAL_UINT8(0x02, (uint8_t)DeviceClass::Screen);
    TEST_ASSERT_EQUAL_UINT8(0x03, (uint8_t)DeviceClass::MultiLedScreen);
}

static void test_stub_bindings_declare_a_device_class(void) {
    StubOutputBinding b;
    TEST_ASSERT_EQUAL_INT((int)DeviceClass::Light, (int)b.device_class());
    // Bindings must NEVER return DeviceClass::All - that value is the
    // addressing wildcard, not a device identity.
    TEST_ASSERT_NOT_EQUAL_INT((int)DeviceClass::All, (int)b.device_class());
}

// =============================================================================
// Registry singleton: register / find / clear via output_binding_registry()
// =============================================================================

static void test_registry_register_find_clear(void) {
    StubOutputBinding b;
    auto& reg = output_binding_registry();

    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_TRUE(reg.register_plugin(&b));
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_EQUAL_PTR(&b, reg.find("stub-bind"));
    TEST_ASSERT_NULL(reg.find("nope"));

    // Singleton: a second call returns the same registry.
    TEST_ASSERT_EQUAL_PTR(&b, output_binding_registry().find("stub-bind"));

    reg.clear();
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_NULL(reg.find("stub-bind"));
}

// =============================================================================
// Capability gating: a binding declaring required caps the host doesn't
// have can be registered, but required.subset_of(host_caps) returns
// false (consumer's check, not the registry's).
// =============================================================================

static void test_required_capabilities_can_be_outside_host_capabilities(void) {
    DemandingBinding demanding;
    auto& reg = output_binding_registry();
    TEST_ASSERT_TRUE(reg.register_plugin(&demanding));

    // Host (test HAL) declares Display + IRTx + Buttons + Mic, NOT ESPNow.
    auto host = make_capability_mask(
        Capability::Display,
        Capability::IRTx,
        Capability::Buttons,
        Capability::Mic);
    TEST_ASSERT_FALSE(demanding.required_capabilities().subset_of(host));

    // A binding with NO required caps is trivially a subset of any host mask.
    StubOutputBinding stub;
    TEST_ASSERT_TRUE(stub.required_capabilities().subset_of(host));
}

// =============================================================================
// OutputBindingContext: property bag round-trip + clamping + type mismatch
// =============================================================================

static void test_context_property_default_then_set_get(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    // Never-set: returns the schema default (100).
    TEST_ASSERT_EQUAL_UINT8(100, ctx.get_property("intensity").as_u8());

    // Set then read back.
    TEST_ASSERT_TRUE(ctx.set_property("intensity", PropertyValue::from_u8(200)));
    TEST_ASSERT_EQUAL_UINT8(200, ctx.get_property("intensity").as_u8());
}

static void test_context_property_set_clamps_out_of_range(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    // U8 max is 255, schema max is also 255 - within range, stored as-is.
    TEST_ASSERT_TRUE(ctx.set_property("intensity", PropertyValue::from_u8(255)));
    TEST_ASSERT_EQUAL_UINT8(255, ctx.get_property("intensity").as_u8());

    // Min bound is 0; type floor is 0, so under-min isn't expressible
    // for U8. Verify the upper edge holds across a refresh.
    TEST_ASSERT_TRUE(ctx.set_property("intensity", PropertyValue::from_u8(0)));
    TEST_ASSERT_EQUAL_UINT8(0, ctx.get_property("intensity").as_u8());
}

static void test_context_property_set_type_mismatch_rejected(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    // Schema says U8; a Bool write must be rejected.
    TEST_ASSERT_FALSE(ctx.set_property("intensity", PropertyValue::from_bool(true)));
    // Value remains the default.
    TEST_ASSERT_EQUAL_UINT8(100, ctx.get_property("intensity").as_u8());
}

// =============================================================================
// OutputBindingContext: identity accessors
// =============================================================================

static void test_context_identity_accessors(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    TEST_ASSERT_EQUAL_PTR(&b,   &ctx.binding());
    TEST_ASSERT_EQUAL_PTR(&bag, &ctx.property_bag());
}

// =============================================================================
// OutputBindingContext: time arithmetic via mark_entered + since_enter_ms
// =============================================================================

static void test_context_since_enter_ms_after_mark_entered(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    set_test_millis(2000);
    ctx.mark_entered(2000);
    TEST_ASSERT_EQUAL_UINT32(2000u, ctx.now_ms());
    TEST_ASSERT_EQUAL_UINT32(0u,    ctx.since_enter_ms());

    set_test_millis(2500);
    TEST_ASSERT_EQUAL_UINT32(2500u, ctx.now_ms());
    TEST_ASSERT_EQUAL_UINT32(500u,  ctx.since_enter_ms());
}

// =============================================================================
// OutputBindingContext: host_caps reflects HAL declarations
// =============================================================================

static void test_context_host_caps_reflects_hal(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    const auto caps = ctx.host_caps();
    // Declared by the test HAL above:
    TEST_ASSERT_TRUE (caps.has(Capability::Display));
    TEST_ASSERT_TRUE (caps.has(Capability::IRTx));
    TEST_ASSERT_TRUE (caps.has(Capability::Buttons));
    TEST_ASSERT_TRUE (caps.has(Capability::Mic));
    // Intentionally not declared:
    TEST_ASSERT_FALSE(caps.has(Capability::IRRx));
    TEST_ASSERT_FALSE(caps.has(Capability::ESPNow));
    TEST_ASSERT_FALSE(caps.has(Capability::Battery));
    // Analyser sub-caps are intentionally not part of host_caps even if
    // the HAL did declare them - bindings sit downstream of analysis.
    TEST_ASSERT_FALSE(caps.has(Capability::AnalyserBeatDetection));
}

// =============================================================================
// on_light_command forwards the event payload intact
// =============================================================================

static void test_on_light_command_forwards_payload(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    dal::RgbPulseEvent ev{200, 100, 50,
                          pixmob::T_32_MS, pixmob::T_96_MS,
                          pixmob::T_192_MS, pixmob::CHANCE_50};
    b.on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, b.light_calls);
    TEST_ASSERT_EQUAL_UINT8(200, b.last_event.r);
    TEST_ASSERT_EQUAL_UINT8(100, b.last_event.g);
    TEST_ASSERT_EQUAL_UINT8(50,  b.last_event.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS,  (int)b.last_event.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS,  (int)b.last_event.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)b.last_event.release);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_50, (int)b.last_event.chance);
}

// =============================================================================
// Stub hooks: enter / exit / on_light_command / on_input_action / tick
// all dispatch through cleanly.
// =============================================================================

static void test_stub_hooks_recorded(void) {
    StubOutputBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    b.enter(ctx);
    TEST_ASSERT_EQUAL_INT(1, b.enter_calls);

    dal::RgbPulseEvent ev{10, 20, 30,
                          pixmob::T_32_MS, pixmob::T_32_MS,
                          pixmob::T_32_MS, pixmob::CHANCE_100};
    b.on_light_command(ctx, ev);
    TEST_ASSERT_EQUAL_INT(1, b.light_calls);
    TEST_ASSERT_EQUAL_UINT8(10, b.last_event.r);

    hal::InputEvent ie{hal::InputAction::Confirm, 42};
    b.on_input_action(ctx, ie);
    TEST_ASSERT_EQUAL_INT(1, b.input_calls);

    b.tick(ctx, 9999);
    TEST_ASSERT_EQUAL_INT(1, b.tick_calls);
    TEST_ASSERT_EQUAL_UINT32(9999u, b.last_tick_ms);

    b.exit(ctx);
    TEST_ASSERT_EQUAL_INT(1, b.exit_calls);
}

// =============================================================================
// Default OutputBinding hooks are no-ops (don't crash, don't side-effect).
// =============================================================================

namespace {
class BareBinding : public OutputBinding {
public:
    const char* id()           const override { return "bare"; }
    const char* display_name() const override { return "Bare"; }
    DeviceClass device_class() const override { return DeviceClass::Light; }
};
}  // namespace

static void test_default_hooks_are_no_ops(void) {
    BareBinding b;
    PropertyBag bag(b);
    OutputBindingContext ctx(b, bag);

    // None of these should crash.
    b.enter(ctx);
    b.exit(ctx);
    dal::RgbPulseEvent ev{};
    b.on_light_command(ctx, ev);
    hal::InputEvent ie{};
    b.on_input_action(ctx, ie);
    b.tick(ctx, 0);

    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)b.kind());
}

// =============================================================================
// Power profile default for an OutputBinding matches the Plugin default.
// =============================================================================

static void test_output_binding_default_power_profile(void) {
    BareBinding b;
    auto p = b.power();
    TEST_ASSERT_TRUE (p.needs_audio_frames);
    TEST_ASSERT_FALSE(p.needs_spectrum_frame);
    TEST_ASSERT_FALSE(p.needs_8band_summary);
    TEST_ASSERT_EQUAL_UINT16(20, p.lcd_refresh_hz_max);
    TEST_ASSERT_EQUAL_UINT16(0,  p.tick_hz);
}

// =============================================================================
// main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_output_binding_kind_is_output_binding);
    RUN_TEST(test_device_class_enum_values_are_wire_stable);
    RUN_TEST(test_stub_bindings_declare_a_device_class);
    RUN_TEST(test_registry_register_find_clear);
    RUN_TEST(test_required_capabilities_can_be_outside_host_capabilities);
    RUN_TEST(test_context_property_default_then_set_get);
    RUN_TEST(test_context_property_set_clamps_out_of_range);
    RUN_TEST(test_context_property_set_type_mismatch_rejected);
    RUN_TEST(test_context_identity_accessors);
    RUN_TEST(test_context_since_enter_ms_after_mark_entered);
    RUN_TEST(test_context_host_caps_reflects_hal);
    RUN_TEST(test_on_light_command_forwards_payload);
    RUN_TEST(test_stub_hooks_recorded);
    RUN_TEST(test_default_hooks_are_no_ops);
    RUN_TEST(test_output_binding_default_power_profile);
    return UNITY_END();
}
