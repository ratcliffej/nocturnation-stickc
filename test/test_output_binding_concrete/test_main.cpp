// Native test: concrete OutputBindings (Epic 4.6 Block 9).
//
// Exercises the two bindings that landed in Block 9 -
// LocalDisplayBinding and PixMobIrBinding - against recording
// drivers registered on the standard "local", "all-pixmobs", and
// "group-N" targets. The legacy slv_ir_grp -> "group" NVS migration
// is also covered here because it requires the binding's property
// bag to be present (i.e. linked into the same env).
//
// Provides a HAL backend with Display + IRTx so both bindings'
// required_capabilities pass the gate. AnalyserBeatDetection is
// intentionally absent - bindings sit downstream of analysis and
// don't care.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "output_bindings/output_binding.h"
#include "output_bindings/output_binding_context.h"
#include "output_bindings/output_binding_registry.h"
#include "output_bindings/local_display.h"
#include "output_bindings/pixmob_ir.h"
#include "../../src/modes/persistence.h"

// =============================================================================
// Native millis() seam (mirrors test_output_binding pattern)
// =============================================================================
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend - Display + IRTx, no others
// =============================================================================

namespace nocturnation {
namespace hal {

class StubDisplay : public Display {
public:
    void begin() override {}
    void set_rotation(uint8_t) override {}
    int  width()  const override { return 240; }
    int  height() const override { return 135; }
    void clear(uint16_t) override {}
    void fill_rect(int, int, int, int, uint16_t) override {}
    void draw_rect(int, int, int, int, uint16_t) override {}
    void draw_hline(int, int, int, uint16_t) override {}
    void draw_vline(int, int, int, uint16_t) override {}
    void set_text_color(uint16_t, uint16_t) override {}
    void set_text_size(uint8_t) override {}
    void draw_text(int, int, const char*) override {}
    void flush() override {}
    bool begin_buffered_paint(int, int, int, int) override { return false; }
    void end_buffered_paint() override {}
};

static StubDisplay s_stub_display;

static constexpr Capability kCapabilities[] = {
    Capability::Display,
    Capability::IRTx,
};
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()    { return kCapabilities; }
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
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Test fixture
// =============================================================================

using namespace nocturnation;
using nocturnation::dal::RgbPulseEvent;
using nocturnation::hal::DeviceClass;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::hal::Capability;
using nocturnation::hal::make_capability_mask;
using nocturnation::output_bindings::LocalDisplayBinding;
using nocturnation::output_bindings::PixMobIrBinding;
using nocturnation::output_bindings::OutputBinding;
using nocturnation::output_bindings::OutputBindingContext;
using nocturnation::output_bindings::local_display_instance;
using nocturnation::output_bindings::local_display_property_bag;
using nocturnation::output_bindings::local_display_context;
using nocturnation::output_bindings::pixmob_ir_instance;
using nocturnation::output_bindings::pixmob_ir_property_bag;
using nocturnation::output_bindings::pixmob_ir_context;
using nocturnation::output_bindings::output_binding_registry;

namespace {

// Records every send on the "ir-pixmob" transport, keyed by the
// group_id DAL::render_fx threads through from the target device's
// group registration. DAL::begin() registers "all-pixmobs" at group
// 0 and "group-N" at group N for N in 1..5, so we can assert which
// virtual device fired by examining last_group_id_.
class RecordingIrDriver : public dal::Driver {
public:
    const char* transport_name() const override { return "ir-pixmob"; }
    bool        begin()                override { return true; }

    bool send(uint8_t group_id, const RgbPulseEvent& ev) override {
        ++rgb_pulse_count_;
        last_group_id_ = group_id;
        last_rgb_pulse_ = ev;
        return true;
    }

    void reset() {
        rgb_pulse_count_ = 0;
        last_group_id_   = 0xFF;
        last_rgb_pulse_  = RgbPulseEvent{};
    }

    int           rgb_pulse_count() const { return rgb_pulse_count_; }
    uint8_t       last_group_id()   const { return last_group_id_; }
    RgbPulseEvent last_rgb_pulse()  const { return last_rgb_pulse_; }

private:
    int           rgb_pulse_count_ = 0;
    uint8_t       last_group_id_   = 0xFF;
    RgbPulseEvent last_rgb_pulse_  = {};
};

RecordingIrDriver g_ir_driver;

}  // namespace

void setUp(void) {
    set_test_millis(0);
    PropertyBag::clear_for_tests();
    output_binding_registry().clear();
    g_ir_driver.reset();
    dal::DAL::begin();
    // Register the test IR driver AFTER DAL::begin so it claims the
    // "ir-pixmob" transport. The firmware ir-pixmob driver refuses
    // registration without HAL IRTx wiring (HAL::ir_tx() returns
    // nullptr here), so the transport is free for the test driver
    // to claim and DAL::render_fx routes every "all-pixmobs" /
    // "group-N" target through this single recorder with the
    // group_id threaded as the send() argument.
    dal::DAL::register_driver(&g_ir_driver);
}

void tearDown(void) {}

// =============================================================================
// LocalDisplayBinding identity / schema / caps
// =============================================================================

static void test_local_display_identity(void) {
    LocalDisplayBinding* b = local_display_instance();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("local-display", b->id());
    TEST_ASSERT_NOT_NULL(b->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)b->kind());
    TEST_ASSERT_EQUAL_INT((int)DeviceClass::Screen, (int)b->device_class());
    // No properties initially.
    TEST_ASSERT_EQUAL_size_t(0, b->properties().size);
}

static void test_local_display_requires_display_capability(void) {
    LocalDisplayBinding* b = local_display_instance();
    const auto req = b->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::Display));
    // Host (test HAL) has Display + IRTx, so the requirement opens.
    const auto host = make_capability_mask(Capability::Display, Capability::IRTx);
    TEST_ASSERT_TRUE(req.subset_of(host));
    // A host without Display fails the gate.
    const auto host_no_display = make_capability_mask(Capability::IRTx);
    TEST_ASSERT_FALSE(req.subset_of(host_no_display));
}

// =============================================================================
// PixMobIrBinding identity / schema / caps
// =============================================================================

static void test_pixmob_ir_identity(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("pixmob-ir", b->id());
    TEST_ASSERT_EQUAL_INT((int)DeviceClass::Light, (int)b->device_class());
    TEST_ASSERT_NOT_NULL(b->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)b->kind());
}

static void test_pixmob_ir_property_schema(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    auto props = b->properties();
    TEST_ASSERT_EQUAL_size_t(1, props.size);
    const auto& def = props[0];
    TEST_ASSERT_EQUAL_STRING("group", def.key);
    TEST_ASSERT_EQUAL_INT((int)PropertyType::Enum, (int)def.type);
    TEST_ASSERT_EQUAL_UINT8(0,  def.min_value.as_enum());
    TEST_ASSERT_EQUAL_UINT8(31, def.max_value.as_enum());      // Epic 4.65 Block 6: full 0..31
    TEST_ASSERT_EQUAL_UINT8(0,  def.default_value.as_enum());  // default broadcast
    TEST_ASSERT_NOT_NULL(def.enum_names);
    TEST_ASSERT_EQUAL_STRING("All",      def.enum_names[0]);
    TEST_ASSERT_EQUAL_STRING("Group 1",  def.enum_names[1]);
    TEST_ASSERT_EQUAL_STRING("Group 5",  def.enum_names[5]);
    TEST_ASSERT_EQUAL_STRING("Group 31", def.enum_names[31]);
}

static void test_pixmob_ir_requires_irtx_capability(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    const auto req = b->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::IRTx));
    const auto host = make_capability_mask(Capability::Display, Capability::IRTx);
    TEST_ASSERT_TRUE(req.subset_of(host));
    const auto host_no_ir = make_capability_mask(Capability::Display);
    TEST_ASSERT_FALSE(req.subset_of(host_no_ir));
}

// =============================================================================
// PixMobIrBinding default power profile (no audio / tick).
// =============================================================================

static void test_pixmob_ir_default_power(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    auto p = b->power();
    // Default profile - event-driven, ticks off.
    TEST_ASSERT_EQUAL_UINT16(0, p.tick_hz);
}

// =============================================================================
// LocalDisplayBinding::on_light_command fires render_fx("local", ev)
// =============================================================================
//
// LocalDriver is registered by DAL::begin() and claims the "local"
// target. We measure the fire by reading DAL::driver_send_count("local")
// delta before and after - the same technique test_beat_pulse uses.

static void test_local_display_fires_local(void) {
    LocalDisplayBinding*  b   = local_display_instance();
    OutputBindingContext& ctx = local_display_context();

    const uint32_t before = dal::DAL::driver_send_count("local");
    RgbPulseEvent ev{0x11, 0x22, 0x33,
                     pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_192_MS, pixmob::CHANCE_100};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_UINT32(before + 1, dal::DAL::driver_send_count("local"));
}

// =============================================================================
// PixMobIrBinding::on_light_command maps group->target name
// =============================================================================

// group=0 -> "all-pixmobs" target, which DAL::begin() registered at
// group_id=0 on the "ir-pixmob" transport.
static void test_pixmob_ir_group_zero_fires_all_pixmobs(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(0)));

    RgbPulseEvent ev{0xAA, 0xBB, 0xCC,
                     pixmob::T_192_MS, pixmob::T_480_MS,
                     pixmob::T_480_MS, pixmob::CHANCE_50};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_ir_driver.last_group_id());

    auto last = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xAA, last.r);
    TEST_ASSERT_EQUAL_UINT8(0xBB, last.g);
    TEST_ASSERT_EQUAL_UINT8(0xCC, last.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)last.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_480_MS, (int)last.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_480_MS, (int)last.release);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_50, (int)last.chance);
}

// group=2 -> "group-2" target, registered at group_id=2.
static void test_pixmob_ir_group_two_fires_group_2(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(2)));

    RgbPulseEvent ev{0x10, 0x20, 0x30,
                     pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_96_MS, pixmob::CHANCE_100};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(2, g_ir_driver.last_group_id());

    auto last = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0x10, last.r);
    TEST_ASSERT_EQUAL_UINT8(0x20, last.g);
    TEST_ASSERT_EQUAL_UINT8(0x30, last.b);
}

// group=5 -> "group-5" target, registered at group_id=5.
static void test_pixmob_ir_group_five_fires_group_5(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(5)));

    RgbPulseEvent ev{0xFE, 0xDC, 0xBA,
                     pixmob::T_32_MS, pixmob::T_32_MS,
                     pixmob::T_32_MS, pixmob::CHANCE_100};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(5, g_ir_driver.last_group_id());

    auto last = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFE, last.r);
    TEST_ASSERT_EQUAL_UINT8(0xDC, last.g);
    TEST_ASSERT_EQUAL_UINT8(0xBA, last.b);
}

// Out-of-range group writes are clamped by PropertyBag bounds; a
// write of 99 lands as enum max (31 post-Epic-4.65 Block 6), which
// then routes to group-31.
static void test_pixmob_ir_group_clamps_out_of_range(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(99)));
    TEST_ASSERT_EQUAL_UINT8(31, ctx.get_property("group").as_enum());

    RgbPulseEvent ev{};
    b->on_light_command(ctx, ev);
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(31, g_ir_driver.last_group_id());
}

// =============================================================================
// Singleton registration via the registry.
// =============================================================================

static void test_registry_register_both(void) {
    auto& reg = output_binding_registry();
    TEST_ASSERT_TRUE(reg.register_plugin(local_display_instance()));
    TEST_ASSERT_TRUE(reg.register_plugin(pixmob_ir_instance()));
    TEST_ASSERT_EQUAL_size_t(2, reg.count());
    TEST_ASSERT_EQUAL_PTR(local_display_instance(),
                           reg.find("local-display"));
    TEST_ASSERT_EQUAL_PTR(pixmob_ir_instance(),
                           reg.find("pixmob-ir"));
}

// =============================================================================
// Fan-out: both bindings registered and entered, a single on_light_command
// per binding fires both transport surfaces with the SAME byte-identical
// RgbPulseEvent. This is the SlaveMode shell's primary behaviour
// (preserved byte-for-byte from the pre-Block-9 render_light() path).
// =============================================================================

// Epic 4.65 Block 5: relay flag distinguishes pass-through bindings
// (PixMobIr) from local bindings (LocalDisplay). The SlaveMode filter
// uses this to decide whether to apply the slv_group check.
static void test_is_relay_flag(void) {
    TEST_ASSERT_FALSE(local_display_instance()->is_relay());
    TEST_ASSERT_TRUE (pixmob_ir_instance()->is_relay());
}

// Epic 4.65 Block 5: PixMobIrBinding takes its IR group code from the
// inbound target_group threaded through OutputBindingContext, falling
// back to its configured "group" property only when target_group is 0
// (broadcast). Preserves the pre-Epic-4.65 broadcast behaviour while
// enabling the new class:group relay model.
static void test_pixmob_ir_uses_current_target_group_when_nonzero(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    // Configure the binding's default group to 2, but feed an inbound
    // target_group of 5 - the inbound wins, IR fires at group 5.
    TEST_ASSERT_TRUE(pixmob_ir_property_bag().set(
        "group", PropertyValue::from_enum(2)));
    ctx.set_current_target(/*target_class=*/0x01, /*target_group=*/5);

    g_ir_driver.reset();
    RgbPulseEvent ev{0x11, 0x22, 0x33, pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_96_MS, pixmob::CHANCE_100};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(5, g_ir_driver.last_group_id());
}

static void test_pixmob_ir_falls_back_to_property_when_target_group_zero(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    // Configure group=4, feed inbound target_group=0 (broadcast).
    // Expect IR at the configured default (4), not 0.
    TEST_ASSERT_TRUE(pixmob_ir_property_bag().set(
        "group", PropertyValue::from_enum(4)));
    ctx.set_current_target(/*target_class=*/0x00, /*target_group=*/0);

    g_ir_driver.reset();
    RgbPulseEvent ev{0x10, 0x20, 0x30, pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_96_MS, pixmob::CHANCE_100};
    b->on_light_command(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(4, g_ir_driver.last_group_id());
}

// Epic 4.65 Block 5: slv_group NVS round-trip. The native persistence
// stub stores in a process-static, mirroring the Arduino Preferences
// path for SlaveMode + ConfigMode read/write.
static void test_slv_group_nvs_round_trip(void) {
    nocturnation::modes::persistence::save_slv_group(7);
    TEST_ASSERT_EQUAL_UINT8(7, nocturnation::modes::persistence::load_slv_group());
    nocturnation::modes::persistence::save_slv_group(0);
    TEST_ASSERT_EQUAL_UINT8(0, nocturnation::modes::persistence::load_slv_group());
}

static void test_fan_out_both_bindings_fire_with_same_event(void) {
    auto& reg = output_binding_registry();
    reg.register_plugin(local_display_instance());
    reg.register_plugin(pixmob_ir_instance());

    // Set group=3 so the PixMob path targets "group-3" (group_id=3).
    TEST_ASSERT_TRUE(pixmob_ir_property_bag().set(
        "group", PropertyValue::from_enum(3)));

    const uint32_t local_before = dal::DAL::driver_send_count("local");

    RgbPulseEvent ev{0x42, 0x84, 0xC6,
                     pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_96_MS, pixmob::CHANCE_100};

    // Manual fan-out using the same ev for both - this mirrors what
    // SlaveMode::fan_out_light_command does over the active_bindings_
    // list.
    local_display_instance()->on_light_command(local_display_context(), ev);
    pixmob_ir_instance()->on_light_command(pixmob_ir_context(),       ev);

    // Local target fired once.
    TEST_ASSERT_EQUAL_UINT32(local_before + 1, dal::DAL::driver_send_count("local"));
    // IR target fired once at group 3 with the same byte-identical
    // event (r/g/b + envelope).
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(3, g_ir_driver.last_group_id());
    auto last = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0x42, last.r);
    TEST_ASSERT_EQUAL_UINT8(0x84, last.g);
    TEST_ASSERT_EQUAL_UINT8(0xC6, last.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS, (int)last.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)last.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)last.release);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_100, (int)last.chance);
}

// =============================================================================
// Selective registration: skipping a binding means only the registered
// one fires. Covers the "operator disabled this binding" path.
// =============================================================================

static void test_only_local_registered_skips_pixmob(void) {
    auto& reg = output_binding_registry();
    reg.register_plugin(local_display_instance());
    // PixMobIrBinding deliberately NOT registered.

    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_NULL(reg.find("pixmob-ir"));
    TEST_ASSERT_NOT_NULL(reg.find("local-display"));
}

// =============================================================================
// Property bag persists across SlaveMode-style re-enters (via the binding's
// own singleton bag). Demonstrates the operator setting is sticky.
// =============================================================================

static void test_group_property_persists_across_clear(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(4)));
    TEST_ASSERT_EQUAL_UINT8(4, ctx.get_property("group").as_enum());

    // clear_for_tests resets the native store; bag's "group" now falls
    // back to the schema default (0). On real hardware (NVS-backed),
    // the value would persist across power cycles via the
    // "nb_pixmob-ir" namespace.
    PropertyBag::clear_for_tests();
    TEST_ASSERT_EQUAL_UINT8(0, ctx.get_property("group").as_enum());

    // And a fresh write sticks.
    TEST_ASSERT_TRUE(ctx.set_property("group", PropertyValue::from_enum(1)));
    TEST_ASSERT_EQUAL_UINT8(1, ctx.get_property("group").as_enum());

    (void)b;
}

// =============================================================================
// NVS migration shim: seed a legacy slv_ir_grp value, run the migration,
// assert it lands in PixMobIrBinding's bag. The native build's
// persistence helpers expose seed/clear test seams.
// =============================================================================

static void test_migration_seeds_pixmob_group_property(void) {
    PropertyBag::clear_for_tests();
    modes::persistence::test_seam::clear_native_persistence();

    // Seed legacy key value 3.
    modes::persistence::test_seam::seed_legacy_slv_ir_grp(3);

    // Run the migration.
    modes::persistence::migrate_legacy_nvs_keys();

    // PixMobIrBinding's bag now has "group" == 3.
    TEST_ASSERT_EQUAL_UINT8(3,
        pixmob_ir_property_bag().get("group").as_enum());

    // Second call is a no-op (legacy state was consumed). Setting the
    // bag to 1 and re-running the migration must NOT overwrite it back
    // to 3.
    TEST_ASSERT_TRUE(pixmob_ir_property_bag().set(
        "group", PropertyValue::from_enum(1)));
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(1,
        pixmob_ir_property_bag().get("group").as_enum());
}

// Out-of-range legacy values get clamped to 0 by the migration's
// `if (g > 31) g = 0` defensive guard. The cap widened from 5 to 31
// in Epic 4.65 Block 6 (PixMob protocol's native range); legacy values
// that were already valid (<=5) migrate unchanged.
static void test_migration_clamps_out_of_range_legacy_value(void) {
    PropertyBag::clear_for_tests();
    modes::persistence::test_seam::clear_native_persistence();

    modes::persistence::test_seam::seed_legacy_slv_ir_grp(99);
    modes::persistence::migrate_legacy_nvs_keys();

    TEST_ASSERT_EQUAL_UINT8(0,
        pixmob_ir_property_bag().get("group").as_enum());
}

// With no legacy key present, the migration is a no-op and the bag
// retains whatever value it had.
static void test_migration_no_op_when_legacy_absent(void) {
    PropertyBag::clear_for_tests();
    modes::persistence::test_seam::clear_native_persistence();

    TEST_ASSERT_TRUE(pixmob_ir_property_bag().set(
        "group", PropertyValue::from_enum(2)));
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(2,
        pixmob_ir_property_bag().get("group").as_enum());
}

// =============================================================================
// Slave persistence helpers (channel + repeat) - sanity round-trip via the
// shared persistence module helpers added in Block 9.
// =============================================================================

static void test_slave_channel_round_trip(void) {
    modes::persistence::test_seam::clear_native_persistence();
    TEST_ASSERT_EQUAL_UINT8(0, modes::persistence::load_slave_channel());

    modes::persistence::save_slave_channel(11);
    TEST_ASSERT_EQUAL_UINT8(11, modes::persistence::load_slave_channel());

    // Invalid value clamps to 0 (auto).
    modes::persistence::save_slave_channel(7);
    TEST_ASSERT_EQUAL_UINT8(0, modes::persistence::load_slave_channel());
}

static void test_slave_repeat_round_trip(void) {
    modes::persistence::test_seam::clear_native_persistence();
    TEST_ASSERT_FALSE(modes::persistence::load_slave_repeat_enabled());

    modes::persistence::save_slave_repeat_enabled(true);
    TEST_ASSERT_TRUE(modes::persistence::load_slave_repeat_enabled());

    modes::persistence::save_slave_repeat_enabled(false);
    TEST_ASSERT_FALSE(modes::persistence::load_slave_repeat_enabled());
}

// =============================================================================
// main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_local_display_identity);
    RUN_TEST(test_local_display_requires_display_capability);
    RUN_TEST(test_pixmob_ir_identity);
    RUN_TEST(test_pixmob_ir_property_schema);
    RUN_TEST(test_pixmob_ir_requires_irtx_capability);
    RUN_TEST(test_pixmob_ir_default_power);
    RUN_TEST(test_local_display_fires_local);
    RUN_TEST(test_pixmob_ir_group_zero_fires_all_pixmobs);
    RUN_TEST(test_pixmob_ir_group_two_fires_group_2);
    RUN_TEST(test_pixmob_ir_group_five_fires_group_5);
    RUN_TEST(test_pixmob_ir_group_clamps_out_of_range);
    RUN_TEST(test_registry_register_both);
    // Block 5 - relay flag + PixMobIr context-driven group
    RUN_TEST(test_is_relay_flag);
    RUN_TEST(test_pixmob_ir_uses_current_target_group_when_nonzero);
    RUN_TEST(test_pixmob_ir_falls_back_to_property_when_target_group_zero);
    RUN_TEST(test_slv_group_nvs_round_trip);
    RUN_TEST(test_fan_out_both_bindings_fire_with_same_event);
    RUN_TEST(test_only_local_registered_skips_pixmob);
    RUN_TEST(test_group_property_persists_across_clear);
    RUN_TEST(test_migration_seeds_pixmob_group_property);
    RUN_TEST(test_migration_clamps_out_of_range_legacy_value);
    RUN_TEST(test_migration_no_op_when_legacy_absent);
    RUN_TEST(test_slave_channel_round_trip);
    RUN_TEST(test_slave_repeat_round_trip);
    return UNITY_END();
}
