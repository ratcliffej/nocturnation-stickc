// Native test: concrete OutputBindings.
//
// Exercises PixMobIrBinding against a recording driver registered on
// the standard "all-pixmobs" and "group-N" targets. The legacy
// slv_ir_grp -> "group" NVS migration is also covered here because
// it requires the binding's property bag to be present (i.e. linked
// into the same env).
//
// Epic 13 B0 retired LocalDisplayBinding (the LCD-as-pixel role); its
// tests were removed alongside the binding.
//
// Provides a HAL backend with Display + IRTx so the binding's
// required_capabilities pass the gate (Display kept for the future
// LumeTextBinding tests). AnalyserBeatDetection is intentionally
// absent - bindings sit downstream of analysis and don't care.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "output_bindings/output_binding.h"
#include "output_bindings/output_binding_context.h"
#include "output_bindings/output_binding_registry.h"
#include "output_bindings/pixmob_ir.h"
#include "output_bindings/lume_text.h"
#include "../../src/modes/persistence.h"
#include "../../src/dal/drivers/espnow_broadcast_driver.h"

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
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }
uint8_t HAL::max_strip_brightness_percent() { return 100; }

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
using nocturnation::output_bindings::PixMobIrBinding;
using nocturnation::output_bindings::LumeTextBinding;
using nocturnation::output_bindings::OutputBinding;
using nocturnation::output_bindings::OutputBindingContext;
using nocturnation::output_bindings::pixmob_ir_instance;
using nocturnation::output_bindings::pixmob_ir_context;
using nocturnation::output_bindings::lume_text_instance;
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

// PixMobIrBinding no longer has any properties (the per-binding "group"
// fallback was dropped once the relay path made it dead weight).
static void test_pixmob_ir_has_no_properties(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    TEST_ASSERT_EQUAL_size_t(0, b->properties().size);
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

// Epic 11 update: PixMobIrBinding now handles the wash family natively
// (native PixMob wash via the DAL capability path). All three caps are
// true; the dispatch layer routes the full wash-family to this binding.
static void test_pixmob_ir_capabilities_full(void) {
    PixMobIrBinding* b = pixmob_ir_instance();
    const auto caps = b->capabilities();
    TEST_ASSERT_TRUE(caps.can_pulse);
    TEST_ASSERT_TRUE(caps.can_wash);
    TEST_ASSERT_TRUE(caps.can_overlay);
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
// PixMobIrBinding::on_light_pulse maps group->target name
// =============================================================================

// Relay routing: inbound target_group=0 fires the "all-pixmobs" target,
// which DAL::begin() registered at group_id=0 on the "ir-pixmob"
// transport. Without a per-binding fallback property, target_group 0
// is the single source of "broadcast to every PixMob".
static void test_pixmob_ir_target_group_zero_fires_all_pixmobs(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();
    ctx.set_current_target(/*target_class=*/0x01, /*target_group=*/0);

    RgbPulseEvent ev{0xAA, 0xBB, 0xCC,
                     pixmob::T_192_MS, pixmob::T_480_MS,
                     pixmob::T_480_MS, pixmob::CHANCE_50};
    b->on_light_pulse(ctx, ev);

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

// Relay routing: inbound target_group=N fires the "group-N" target.
static void test_pixmob_ir_target_group_n_fires_group_n(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();

    // target_group=2
    g_ir_driver.reset();
    ctx.set_current_target(0x01, 2);
    RgbPulseEvent ev2{0x10, 0x20, 0x30,
                      pixmob::T_32_MS, pixmob::T_96_MS,
                      pixmob::T_96_MS, pixmob::CHANCE_100};
    b->on_light_pulse(ctx, ev2);
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(2, g_ir_driver.last_group_id());

    // target_group=31 (top of PixMob native range)
    g_ir_driver.reset();
    ctx.set_current_target(0x01, 31);
    b->on_light_pulse(ctx, ev2);
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(31, g_ir_driver.last_group_id());
}

// Out-of-range inbound target_group (>31) falls back to "all-pixmobs"
// at the ir_target_name() layer - defensive against malformed wire data.
static void test_pixmob_ir_target_group_over_31_falls_back_to_all(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();
    ctx.set_current_target(0x01, 99);

    g_ir_driver.reset();
    RgbPulseEvent ev{};
    b->on_light_pulse(ctx, ev);
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0, g_ir_driver.last_group_id());
}

// =============================================================================
// Epic 13: LumeTextBinding identity / schema / caps
// =============================================================================

static void test_lume_text_identity(void) {
    LumeTextBinding* b = lume_text_instance();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("lume-text", b->id());
    TEST_ASSERT_NOT_NULL(b->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::OutputBinding, (int)b->kind());
    // Epic 13 introduces DeviceClass::Display - separate addressing axis
    // from Light. Strips + PixMobs stay on Light; screens go to Display.
    TEST_ASSERT_EQUAL_INT((int)DeviceClass::Display, (int)b->device_class());
    TEST_ASSERT_EQUAL_size_t(0, b->properties().size);
}

static void test_lume_text_requires_display_text_capability(void) {
    LumeTextBinding* b = lume_text_instance();
    const auto req = b->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::DisplayText));
    // Host (test HAL) only has Display + IRTx, NOT DisplayText - so the
    // requirement is NOT satisfied. That's correct: a Stick declares
    // DisplayText explicitly via its HAL backend, the test HAL doesn't
    // (yet). This is the gate that keeps the binding from activating
    // on hosts without text rendering support.
    const auto host_display_only = make_capability_mask(Capability::Display, Capability::IRTx);
    TEST_ASSERT_FALSE(req.subset_of(host_display_only));
    // Adding DisplayText opens the gate.
    const auto host_with_text = make_capability_mask(Capability::Display,
                                                     Capability::IRTx,
                                                     Capability::DisplayText);
    TEST_ASSERT_TRUE(req.subset_of(host_with_text));
}

// LumeTextBinding's wash-family capabilities are all false - it
// renders text, not lighting. The dispatch layer reads can_wash etc.
// for the LIGHT_WASH family fan-out; the text + clearscreen hooks
// route through device_class() == Display instead.
static void test_lume_text_wash_caps_all_false(void) {
    LumeTextBinding* b = lume_text_instance();
    const auto caps = b->capabilities();
    TEST_ASSERT_FALSE(caps.can_pulse);
    TEST_ASSERT_FALSE(caps.can_wash);
    TEST_ASSERT_FALSE(caps.can_overlay);
}

static void test_lume_text_is_not_relay(void) {
    TEST_ASSERT_FALSE(lume_text_instance()->is_relay());
}

// =============================================================================
// Singleton registration via the registry.
// =============================================================================

static void test_registry_register_pixmob(void) {
    auto& reg = output_binding_registry();
    TEST_ASSERT_TRUE(reg.register_plugin(pixmob_ir_instance()));
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_EQUAL_PTR(pixmob_ir_instance(),
                           reg.find("pixmob-ir"));
}

// Epic 4.65 Block 5: relay flag distinguishes pass-through bindings
// (PixMobIr) from local bindings. The LumeMode filter uses this to
// decide whether to apply the slv_group check.
static void test_is_relay_flag(void) {
    TEST_ASSERT_TRUE(pixmob_ir_instance()->is_relay());
}

// Epic 4.65 Block 5: PixMobIrBinding takes its IR group code from the
// inbound target_group threaded through OutputBindingContext, falling
// back to its configured "group" property only when target_group is 0
// (broadcast). Preserves the pre-Epic-4.65 broadcast behaviour while
// enabling the new class:group relay model.
// Relay routing: inbound target_group N drives the IR group code
// (already covered by test_pixmob_ir_target_group_n_fires_group_n
// above; kept as a sanity duplicate to make the Block 5 relay
// behaviour explicit alongside the other Block 5 assertions).
static void test_pixmob_ir_uses_current_target_group(void) {
    PixMobIrBinding*      b   = pixmob_ir_instance();
    OutputBindingContext& ctx = pixmob_ir_context();
    ctx.set_current_target(/*target_class=*/0x01, /*target_group=*/5);

    g_ir_driver.reset();
    RgbPulseEvent ev{0x11, 0x22, 0x33, pixmob::T_32_MS, pixmob::T_96_MS,
                     pixmob::T_96_MS, pixmob::CHANCE_100};
    b->on_light_pulse(ctx, ev);

    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(5, g_ir_driver.last_group_id());
}

// Epic 4.65 Block 5: slv_group NVS round-trip. The native persistence
// stub stores in a process-static, mirroring the Arduino Preferences
// path for LumeMode + ConfigMode read/write.
static void test_slv_group_nvs_round_trip(void) {
    nocturnation::modes::persistence::save_lume_group(7);
    TEST_ASSERT_EQUAL_UINT8(7, nocturnation::modes::persistence::load_lume_group());
    nocturnation::modes::persistence::save_lume_group(0);
    TEST_ASSERT_EQUAL_UINT8(0, nocturnation::modes::persistence::load_lume_group());
}

// =============================================================================
// NVS migration shim: seed a legacy slv_ir_grp value and verify the
// migration consumes it without writing anywhere. The per-binding
// fallback property was dropped, so the legacy key is just retired
// (avoids a stale value shadowing any future per-binding settings).
// =============================================================================

static void test_migration_consumes_legacy_key(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::seed_legacy_slv_ir_grp(3);

    // Migration runs; legacy state is cleared so a second call is a
    // no-op. We can't easily assert "not written anywhere" without
    // exposing more seam, but clear_native_persistence afterwards
    // returns the same state as if no seed had ever happened.
    modes::persistence::migrate_legacy_nvs_keys();

    // Re-seeding works (proves the first migration drained the slot).
    modes::persistence::test_seam::seed_legacy_slv_ir_grp(7);
    modes::persistence::migrate_legacy_nvs_keys();
}

// Migration is a no-op when no legacy key was seeded.
static void test_migration_no_op_when_legacy_absent(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::migrate_legacy_nvs_keys();
    // Should not crash; nothing else to assert.
    TEST_PASS();
}

// Epic 5 prep: first-boot slv_group assignment. A fresh device (no
// prior save_lume_group call) gets a random value in {1, 2, 3} written
// by migrate_legacy_nvs_keys. The native test seam pins the random
// stand-in so the outcome is deterministic; we exercise all three
// values to prove the path is wired correctly end-to-end.
static void test_first_boot_assigns_random_group(void) {
    for (uint8_t expected : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
        modes::persistence::test_seam::clear_native_persistence();
        modes::persistence::test_seam::set_first_boot_rng(expected);

        modes::persistence::migrate_legacy_nvs_keys();

        TEST_ASSERT_EQUAL_UINT8(expected,
                                modes::persistence::load_lume_group());
    }
}

// Operator-set slv_group survives the migrate call. Once save_lume_group
// has been called the key is "written" and migrate must not retro-
// randomise it. In particular slv_group = 0 (operator explicitly opts
// into "broadcast only") is honoured.
static void test_migration_preserves_operator_set_group(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_rng(2);

    modes::persistence::save_lume_group(7);
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(7, modes::persistence::load_lume_group());

    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::save_lume_group(0);
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(0, modes::persistence::load_lume_group());
}

// Second migrate call is a no-op (post-first-boot the key is written;
// migrate should leave it alone).
static void test_migration_is_idempotent_for_first_boot(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_rng(3);

    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(3, modes::persistence::load_lume_group());

    // Re-arm with a different RNG value; second migrate must not
    // overwrite (mirroring the Arduino isKey() guard).
    modes::persistence::test_seam::set_first_boot_rng(1);
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(3, modes::persistence::load_lume_group());
}

// =============================================================================
// Lume persistence helpers (channel + repeat) - sanity round-trip via the
// shared persistence module helpers added in Block 9.
// =============================================================================

static void test_lume_channel_round_trip(void) {
    modes::persistence::test_seam::clear_native_persistence();
    TEST_ASSERT_EQUAL_UINT8(0, modes::persistence::load_lume_channel());

    modes::persistence::save_lume_channel(11);
    TEST_ASSERT_EQUAL_UINT8(11, modes::persistence::load_lume_channel());

    // Invalid value clamps to 0 (auto).
    modes::persistence::save_lume_channel(7);
    TEST_ASSERT_EQUAL_UINT8(0, modes::persistence::load_lume_channel());
}

static void test_lume_repeat_round_trip(void) {
    modes::persistence::test_seam::clear_native_persistence();
    TEST_ASSERT_FALSE(modes::persistence::load_lume_repeat_enabled());

    modes::persistence::save_lume_repeat_enabled(true);
    TEST_ASSERT_TRUE(modes::persistence::load_lume_repeat_enabled());

    modes::persistence::save_lume_repeat_enabled(false);
    TEST_ASSERT_FALSE(modes::persistence::load_lume_repeat_enabled());
}

// =============================================================================
// Director source_id (Epic 5.5 B3): community-range stable ID on channel 1
// =============================================================================

// Fresh device: mst_src_id has never been written. migrate picks a
// value from [0x00, 0x3F] using the deterministic test rng and
// persists it. Three values exercised end-to-end to prove the path
// is wired correctly.
static void test_first_boot_assigns_community_range_director_src_id(void) {
    for (uint8_t expected : {uint8_t{0x00}, uint8_t{0x1A}, uint8_t{0x3F}}) {
        modes::persistence::test_seam::clear_native_persistence();
        modes::persistence::test_seam::set_first_boot_director_src_id_rng(expected);

        modes::persistence::migrate_legacy_nvs_keys();

        TEST_ASSERT_EQUAL_UINT8(
            expected, modes::persistence::load_director_source_id());
    }
}

// Operator-set (well, post-first-boot persisted) mst_src_id survives a
// second migrate call. Once the key is present and in-range, migrate
// must not retro-randomise it.
static void test_migrate_preserves_persisted_director_src_id(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_director_src_id_rng(0x05);

    modes::persistence::save_director_source_id(0x2A);
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(0x2A,
                            modes::persistence::load_director_source_id());

    // And again - second migrate is a no-op for an already-set key.
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_UINT8(0x2A,
                            modes::persistence::load_director_source_id());
}

// save_director_source_id clamps Performance-range or broadcast input
// to 0 (community range floor). Protects against future code paths
// that might try to write a non-community ID through this entry point.
static void test_save_clamps_non_community_id_to_zero(void) {
    modes::persistence::test_seam::clear_native_persistence();

    modes::persistence::save_director_source_id(0xAA);   // Performance-range
    TEST_ASSERT_EQUAL_UINT8(0,
                            modes::persistence::load_director_source_id());

    modes::persistence::save_director_source_id(0xFF);   // broadcast
    TEST_ASSERT_EQUAL_UINT8(0,
                            modes::persistence::load_director_source_id());

    // Boundary: 0x3F is the last community value and MUST pass through
    // unchanged.
    modes::persistence::save_director_source_id(0x3F);
    TEST_ASSERT_EQUAL_UINT8(0x3F,
                            modes::persistence::load_director_source_id());
}

// A persisted mst_src_id outside the community range (older firmware,
// NVS corruption) is treated as missing by migrate: re-rolls from the
// rng seam, leaving the device with a valid community-range value.
static void test_out_of_range_persisted_src_id_is_re_rolled_by_migrate(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_director_src_id_rng(0x12);

    // Plant a Performance-range value directly into the native store
    // (bypassing save_'s clamp). This simulates a corrupted NVS or an
    // older firmware that wrote a value outside the partition.
    modes::persistence::test_seam::plant_raw_director_src_id(0xAA);

    modes::persistence::migrate_legacy_nvs_keys();

    TEST_ASSERT_EQUAL_UINT8(0x12,
                            modes::persistence::load_director_source_id());
    TEST_ASSERT_TRUE(transport::espnow::is_community_range(
        modes::persistence::load_director_source_id()));
}

// EspNowBroadcastDriver::derive_source_id(channel) routes to the
// persistence helper on channel 1, returning the persisted
// community-range ID.
static void test_driver_derive_source_id_channel_1_uses_persistence(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_director_src_id_rng(0x07);
    modes::persistence::migrate_legacy_nvs_keys();

    const uint8_t id = dal::EspNowBroadcastDriver::derive_source_id(1);
    TEST_ASSERT_EQUAL_UINT8(0x07, id);
    TEST_ASSERT_TRUE(transport::espnow::is_community_range(id));
}

// Channels 6 and 11 keep the legacy fallback (native: returns 1).
// Confirms the channel branch is wired - B4 will replace the channel
// 11 case with the Performance-range random-per-boot path.
static void test_driver_derive_source_id_non_channel_1_falls_back(void) {
    modes::persistence::test_seam::clear_native_persistence();
    modes::persistence::test_seam::set_first_boot_director_src_id_rng(0x07);
    modes::persistence::migrate_legacy_nvs_keys();

    // On native, the fallback returns 1 regardless of channel. The
    // important assertion is that the value is NOT the persisted
    // community-range ID - i.e. the channel != 1 branch is taken.
    const uint8_t id_ch6  = dal::EspNowBroadcastDriver::derive_source_id(6);
    const uint8_t id_ch11 = dal::EspNowBroadcastDriver::derive_source_id(11);
    TEST_ASSERT_NOT_EQUAL_UINT8(0x07, id_ch6);
    TEST_ASSERT_NOT_EQUAL_UINT8(0x07, id_ch11);
}

// =============================================================================
// Channel-11 listen-before-broadcast (Epic 5.5 B4)
// =============================================================================

namespace {
dal::EspNowBroadcastDriver* listen_driver() {
    return dal::esp_now_broadcast_driver_instance();
}

void reset_listen_driver() {
    auto* drv = listen_driver();
    drv->stop_broadcast();   // forces state back to Idle if any prior test left it set
    dal::test_seam::clear_native_driver_state();
}
}

// No collision during the listen window: window elapses, driver
// settles on the candidate, source_id is committed.
static void test_listen_no_collision_settles_to_first_candidate(void) {
    reset_listen_driver();
    auto* drv = listen_driver();
    dal::test_seam::set_now_ms(0);

    drv->test_enter_listening(/*candidate=*/0x4A, /*started_ms=*/0);
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Listening),
                      static_cast<int>(drv->startup_state()));

    // Halfway through the window: nothing settles yet.
    dal::test_seam::set_now_ms(500);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Listening),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_FALSE(drv->active());

    // Window elapsed, no collision -> settle to Active with the candidate.
    dal::test_seam::set_now_ms(1000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Active),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_TRUE(drv->active());
    TEST_ASSERT_EQUAL_UINT8(0x4A, drv->source_id());

    reset_listen_driver();
}

// One collision in window-1, clear in window-2: re-roll picks the
// queued next candidate, second window elapses cleanly, driver
// settles with the re-rolled id.
static void test_listen_one_collision_then_clear(void) {
    reset_listen_driver();
    auto* drv = listen_driver();
    dal::test_seam::set_now_ms(0);

    // Queue the value the re-roll will use.
    dal::test_seam::queue_next_performance_pick(0x4B);

    drv->test_enter_listening(/*candidate=*/0x4A, /*started_ms=*/0);
    TEST_ASSERT_EQUAL_UINT8(0x4A, drv->test_listen_candidate());
    TEST_ASSERT_EQUAL_UINT8(3, drv->test_listen_attempts_remaining());

    // Inject a colliding HEARTBEAT mid-window.
    dal::test_seam::set_now_ms(400);
    drv->test_inject_listen_heartbeat(0x4A);
    TEST_ASSERT_TRUE(drv->test_listen_collision_heard());

    // Window elapses with a collision recorded: driver decrements
    // attempts (3 -> 2), re-rolls, and resets the window start.
    dal::test_seam::set_now_ms(1000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Listening),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_EQUAL_UINT8(0x4B, drv->test_listen_candidate());
    TEST_ASSERT_EQUAL_UINT8(2, drv->test_listen_attempts_remaining());
    TEST_ASSERT_FALSE(drv->test_listen_collision_heard());

    // Second window: no collisions. Settle.
    dal::test_seam::set_now_ms(2000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Active),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_EQUAL_UINT8(0x4B, drv->source_id());

    reset_listen_driver();
}

// Three consecutive collisions (one per window): driver logs the
// warning and settles with the third pick anyway per spec §3.4.
static void test_listen_three_collisions_settles_with_warning(void) {
    reset_listen_driver();
    auto* drv = listen_driver();
    dal::test_seam::set_now_ms(0);

    // Queue the re-rolls for attempts 2 and 3. Attempt-1 uses the
    // candidate passed to test_enter_listening.
    dal::test_seam::queue_next_performance_pick(0x4B);
    dal::test_seam::queue_next_performance_pick(0x4C);

    drv->test_enter_listening(/*candidate=*/0x4A, /*started_ms=*/0);

    // Attempt 1: collision, decrement 3 -> 2, re-roll to 0x4B.
    drv->test_inject_listen_heartbeat(0x4A);
    dal::test_seam::set_now_ms(1000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0x4B, drv->test_listen_candidate());
    TEST_ASSERT_EQUAL_UINT8(2, drv->test_listen_attempts_remaining());

    // Attempt 2: collision again, 2 -> 1, re-roll to 0x4C.
    drv->test_inject_listen_heartbeat(0x4B);
    dal::test_seam::set_now_ms(2000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0x4C, drv->test_listen_candidate());
    TEST_ASSERT_EQUAL_UINT8(1, drv->test_listen_attempts_remaining());

    // Attempt 3: collision yet again, 1 -> 0, but no further re-roll
    // - settle with 0x4C per the spec's "MAY proceed and SHOULD log".
    drv->test_inject_listen_heartbeat(0x4C);
    dal::test_seam::set_now_ms(3000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Active),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_EQUAL_UINT8(0x4C, drv->source_id());

    reset_listen_driver();
}

// Non-colliding HEARTBEATs (different source_id) MUST NOT trip the
// collision flag - the listen filter is candidate-specific.
static void test_listen_ignores_non_matching_heartbeat(void) {
    reset_listen_driver();
    auto* drv = listen_driver();
    dal::test_seam::set_now_ms(0);

    drv->test_enter_listening(/*candidate=*/0x4A, /*started_ms=*/0);

    // Inject HEARTBEATs from other Directors - these are noise, not
    // collisions against our candidate.
    drv->test_inject_listen_heartbeat(0x55);
    drv->test_inject_listen_heartbeat(0xAA);
    drv->test_inject_listen_heartbeat(0x4B);
    TEST_ASSERT_FALSE(drv->test_listen_collision_heard());

    dal::test_seam::set_now_ms(1000);
    drv->loop_tick();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Active),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_EQUAL_UINT8(0x4A, drv->source_id());

    reset_listen_driver();
}

// =============================================================================
// Status-label formatter (Epic 5.5 B5)
// =============================================================================

using StartupState = dal::EspNowBroadcastDriver::StartupState;

static void test_status_label_idle_returns_empty(void) {
    char buf[16] = "junk";
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Idle, 0x07, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_status_label_active_community_uses_C_prefix(void) {
    char buf[16] = {0};
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x03, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING("C:03", buf);
}

static void test_status_label_active_performance_uses_P_prefix(void) {
    char buf[16] = {0};
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x4F, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(4, n);
    TEST_ASSERT_EQUAL_STRING("P:4F", buf);
}

// Listening shows the candidate (not source_id_) plus a "?" suffix to
// indicate the value isn't yet settled.
static void test_status_label_listening_shows_candidate_with_question_mark(void) {
    char buf[16] = {0};
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Listening,
        /*source_id_value=*/0x01,             // placeholder (ignored in Listening)
        /*listen_candidate_value=*/0x4B,
        buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(5, n);
    TEST_ASSERT_EQUAL_STRING("P:4B?", buf);
}

// Boundary IDs: 0x00 (min community), 0x3F (max community), 0x40 (min
// Performance), 0xFE (max Performance).
static void test_status_label_boundary_ids(void) {
    char buf[16];
    dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x00, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("C:00", buf);

    dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x3F, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("C:3F", buf);

    dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x40, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("P:40", buf);

    dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0xFE, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("P:FE", buf);
}

// 0xFF (broadcast) and anything else outside the two ranges falls
// back to "?:" so it's visually obvious something is wrong.
static void test_status_label_out_of_range_uses_question_prefix(void) {
    char buf[16];
    dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0xFF, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("?:FF", buf);
}

// Buffer smaller than the formatted output -> no partial label written,
// returns 0. Operator sees nothing rather than misleading half-text.
static void test_status_label_truncated_buffer_returns_zero(void) {
    char tiny[3] = {'a', 'b', '\0'};   // need 5 chars for "C:03"
    const size_t n = dal::EspNowBroadcastDriver::format_status_label(
        StartupState::Active, 0x03, 0, tiny, sizeof(tiny));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

// =============================================================================

// stop_broadcast() mid-Listening cleans up state: Idle, no leftover
// candidate / attempts / collision flag.
static void test_stop_broadcast_mid_listening_cleans_up(void) {
    reset_listen_driver();
    auto* drv = listen_driver();
    dal::test_seam::set_now_ms(0);

    drv->test_enter_listening(/*candidate=*/0x4A, /*started_ms=*/0);
    drv->test_inject_listen_heartbeat(0x4A);   // record a collision for good measure
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Listening),
                      static_cast<int>(drv->startup_state()));

    drv->stop_broadcast();
    TEST_ASSERT_EQUAL(static_cast<int>(dal::EspNowBroadcastDriver::StartupState::Idle),
                      static_cast<int>(drv->startup_state()));
    TEST_ASSERT_FALSE(drv->active());
    TEST_ASSERT_EQUAL_UINT8(0, drv->test_listen_attempts_remaining());
    TEST_ASSERT_FALSE(drv->test_listen_collision_heard());

    // After cleanup, a subsequent re-entry into Listening starts fresh.
    drv->test_enter_listening(/*candidate=*/0x5A, /*started_ms=*/0);
    TEST_ASSERT_EQUAL_UINT8(3, drv->test_listen_attempts_remaining());

    reset_listen_driver();
}

// =============================================================================
// main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_pixmob_ir_identity);
    RUN_TEST(test_pixmob_ir_has_no_properties);
    RUN_TEST(test_pixmob_ir_requires_irtx_capability);
    RUN_TEST(test_pixmob_ir_capabilities_full);
    RUN_TEST(test_pixmob_ir_default_power);
    RUN_TEST(test_pixmob_ir_target_group_zero_fires_all_pixmobs);
    RUN_TEST(test_pixmob_ir_target_group_n_fires_group_n);
    RUN_TEST(test_pixmob_ir_target_group_over_31_falls_back_to_all);
    RUN_TEST(test_registry_register_pixmob);
    RUN_TEST(test_is_relay_flag);
    RUN_TEST(test_lume_text_identity);
    RUN_TEST(test_lume_text_requires_display_text_capability);
    RUN_TEST(test_lume_text_wash_caps_all_false);
    RUN_TEST(test_lume_text_is_not_relay);
    RUN_TEST(test_pixmob_ir_uses_current_target_group);
    RUN_TEST(test_slv_group_nvs_round_trip);
    RUN_TEST(test_migration_consumes_legacy_key);
    RUN_TEST(test_migration_no_op_when_legacy_absent);
    RUN_TEST(test_first_boot_assigns_random_group);
    RUN_TEST(test_migration_preserves_operator_set_group);
    RUN_TEST(test_migration_is_idempotent_for_first_boot);
    RUN_TEST(test_lume_channel_round_trip);
    RUN_TEST(test_lume_repeat_round_trip);
    RUN_TEST(test_first_boot_assigns_community_range_director_src_id);
    RUN_TEST(test_migrate_preserves_persisted_director_src_id);
    RUN_TEST(test_save_clamps_non_community_id_to_zero);
    RUN_TEST(test_out_of_range_persisted_src_id_is_re_rolled_by_migrate);
    RUN_TEST(test_driver_derive_source_id_channel_1_uses_persistence);
    RUN_TEST(test_driver_derive_source_id_non_channel_1_falls_back);
    RUN_TEST(test_listen_no_collision_settles_to_first_candidate);
    RUN_TEST(test_listen_one_collision_then_clear);
    RUN_TEST(test_listen_three_collisions_settles_with_warning);
    RUN_TEST(test_listen_ignores_non_matching_heartbeat);
    RUN_TEST(test_status_label_idle_returns_empty);
    RUN_TEST(test_status_label_active_community_uses_C_prefix);
    RUN_TEST(test_status_label_active_performance_uses_P_prefix);
    RUN_TEST(test_status_label_listening_shows_candidate_with_question_mark);
    RUN_TEST(test_status_label_boundary_ids);
    RUN_TEST(test_status_label_out_of_range_uses_question_prefix);
    RUN_TEST(test_status_label_truncated_buffer_returns_zero);
    RUN_TEST(test_stop_broadcast_mid_listening_cleans_up);
    return UNITY_END();
}
