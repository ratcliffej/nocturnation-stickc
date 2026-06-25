// Native test: Show plug-in framework + SimpleBeatShow (Epic 4.7 Block 1).
//
// Covers the new Director-side Show contract / context / registry plus
// the first concrete Show. The pre-Block-1 BeatPulse fan-out moves
// here; this test exercises the new Show in isolation against recording
// drivers registered on "ir-pixmob" and "esp-now-broadcast" so the
// per-beat render fan-out can be asserted byte-for-byte against the
// pre-migration shape.
//
// Also exercises the active_vis -> active_show NVS migration shim in
// persistence.cpp via the native test seam.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "shows/show.h"
#include "shows/show_context.h"
#include "shows/show_registry.h"
#include "shows/simple_beat_show.h"
#include "../../src/modes/persistence.h"

// =============================================================================
// Native millis() seam
// =============================================================================
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend - Mic + Display + AnalyserBeatDetection. No IRTx /
// ESPNow so the firmware pixmob_ir / espnow drivers refuse registration
// and our test recorders claim the transports. AnalyserSpectrumFrame
// intentionally absent so the capability-mask negation test has a
// missing bit to look for.
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
    Capability::Mic,
    Capability::Display,
    Capability::AnalyserBeatDetection,
    Capability::AnalyserBandSummary,
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
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }
uint8_t HAL::max_strip_brightness_percent() { return 100; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Recording drivers
// =============================================================================

using namespace nocturnation;
using nocturnation::dal::AudioFrameEvent;
using nocturnation::dal::DisplayClearEvent;
using nocturnation::dal::RgbPulseEvent;
using nocturnation::hal::Capability;
using nocturnation::hal::CapabilityMask;
using nocturnation::hal::InputAction;
using nocturnation::hal::InputEvent;
using nocturnation::hal::make_capability_mask;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;
using nocturnation::shows::show_registry;
using nocturnation::shows::Show;
using nocturnation::shows::ShowContext;
using nocturnation::shows::SimpleBeatShow;
using nocturnation::shows::simple_beat_show_context;
using nocturnation::shows::simple_beat_show_instance;
using nocturnation::shows::simple_beat_show_property_bag;

namespace {

class RecordingDriver : public dal::Driver {
public:
    explicit RecordingDriver(const char* name) : name_(name) {}
    const char* transport_name() const override { return name_; }
    bool        begin()                override { return true; }

    bool send(uint8_t, const RgbPulseEvent& ev) override {
        ++rgb_pulse_count_;
        last_rgb_pulse_ = ev;
        return true;
    }
    bool send(uint8_t, const DisplayClearEvent& ev) override {
        ++display_clear_count_;
        last_display_clear_ = ev;
        return true;
    }

    void reset() {
        rgb_pulse_count_     = 0;
        display_clear_count_ = 0;
        last_rgb_pulse_      = RgbPulseEvent{};
        last_display_clear_  = DisplayClearEvent{};
    }

    int               rgb_pulse_count()      const { return rgb_pulse_count_; }
    RgbPulseEvent     last_rgb_pulse()       const { return last_rgb_pulse_; }
    int               display_clear_count()  const { return display_clear_count_; }
    DisplayClearEvent last_display_clear()   const { return last_display_clear_; }

private:
    const char*       name_;
    int               rgb_pulse_count_     = 0;
    int               display_clear_count_ = 0;
    RgbPulseEvent     last_rgb_pulse_      = {};
    DisplayClearEvent last_display_clear_  = {};
};

RecordingDriver g_ir_driver    {"ir-pixmob"};
RecordingDriver g_espnow_driver{"esp-now-broadcast"};

// Stub Show used to exercise the framework hooks in isolation.
class StubShow : public Show {
public:
    const char* id()           const override { return "stub-show"; }
    const char* display_name() const override { return "Stub Show"; }

    void enter(ShowContext&)          override { ++enter_calls; }
    void exit (ShowContext&)          override { ++exit_calls;  }
    void on_audio_frame(ShowContext&, const AudioFrameEvent&) override { ++audio_calls; }
    void on_beat_detected(ShowContext&, uint8_t s) override {
        ++beat_calls;
        last_strength = s;
    }
    void on_input_action(ShowContext&, const InputEvent&) override { ++input_calls; }
    void on_render(ShowContext&)      override { ++render_calls; }

    ShowContext& context() override {
        static PropertyBag bag(*this);
        static ShowContext c(*this, bag);
        return c;
    }

    void reset() {
        enter_calls = exit_calls = audio_calls = beat_calls = 0;
        input_calls = render_calls = 0;
        last_strength = 0;
    }

    int     enter_calls   = 0;
    int     exit_calls    = 0;
    int     audio_calls   = 0;
    int     beat_calls    = 0;
    int     input_calls   = 0;
    int     render_calls  = 0;
    uint8_t last_strength = 0;
};

class DemandingShow : public Show {
public:
    const char* id()           const override { return "demanding"; }
    const char* display_name() const override { return "Demanding"; }
    CapabilityMask required_capabilities() const override {
        return make_capability_mask(Capability::AnalyserSpectrumFrame);
    }
    ShowContext& context() override {
        static PropertyBag bag(*this);
        static ShowContext c(*this, bag);
        return c;
    }
};

}  // namespace

// =============================================================================
// Unity setup / teardown
// =============================================================================

void setUp(void) {
    set_test_millis(0);
    PropertyBag::clear_for_tests();
    show_registry().clear();
    g_ir_driver.reset();
    g_espnow_driver.reset();
    modes::persistence::test_seam::clear_native_persistence();
    // SimpleBeatShow + StubShow contexts are TU-static singletons; reset
    // any paused state so it doesn't leak across tests.
    simple_beat_show_context().set_paused(false);
    dal::DAL::begin();
    dal::DAL::register_driver(&g_ir_driver);
    dal::DAL::register_driver(&g_espnow_driver);
}

void tearDown(void) {}

// =============================================================================
// Framework: kind
// =============================================================================

static void test_show_kind_is_show(void) {
    StubShow s;
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Show, (int)s.kind());

    SimpleBeatShow* sb = simple_beat_show_instance();
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Show, (int)sb->kind());
}

// =============================================================================
// Framework: registry register / find / clear
// =============================================================================

static void test_registry_register_find_clear(void) {
    StubShow s;
    auto& reg = show_registry();

    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_TRUE(reg.register_plugin(&s));
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_EQUAL_PTR(&s, reg.find("stub-show"));
    TEST_ASSERT_NULL(reg.find("nope"));

    // Singleton across calls.
    TEST_ASSERT_EQUAL_PTR(&s, show_registry().find("stub-show"));

    reg.clear();
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_NULL(reg.find("stub-show"));
}

// =============================================================================
// Framework: capability gating - a Show with unmet requirements still
// registers but required.subset_of(host) returns false.
// =============================================================================

static void test_required_capabilities_can_be_outside_host(void) {
    DemandingShow demanding;
    TEST_ASSERT_TRUE(show_registry().register_plugin(&demanding));

    const auto host = make_capability_mask(
        Capability::AnalyserBeatDetection,
        Capability::AnalyserBandSummary);
    TEST_ASSERT_FALSE(demanding.required_capabilities().subset_of(host));

    StubShow stub;
    TEST_ASSERT_TRUE(stub.required_capabilities().subset_of(host));
}

// =============================================================================
// ShowContext: property bag round-trip
// =============================================================================

static void test_context_property_round_trip(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(3));

    auto& ctx = sb->context();
    TEST_ASSERT_EQUAL_UINT8(3, ctx.get_property("color").as_enum());

    TEST_ASSERT_TRUE(ctx.set_property("color", PropertyValue::from_enum(4)));
    TEST_ASSERT_EQUAL_UINT8(4, ctx.get_property("color").as_enum());
    TEST_ASSERT_EQUAL_UINT8(4, bag.get("color").as_enum());
}

// =============================================================================
// SimpleBeatShow: identity / kind / display_name
// =============================================================================

static void test_simple_beat_identity(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    TEST_ASSERT_NOT_NULL(sb);
    TEST_ASSERT_EQUAL_STRING("simple-beat", sb->id());
    TEST_ASSERT_EQUAL_STRING("Simple Beat", sb->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Show, (int)sb->kind());
}

// =============================================================================
// SimpleBeatShow: property schema mirrors BeatPulse's "color" enum.
// =============================================================================

static void test_simple_beat_properties_schema(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto props = sb->properties();
    TEST_ASSERT_EQUAL_size_t(1, props.size);
    const auto& def = props[0];
    TEST_ASSERT_EQUAL_STRING("color", def.key);
    TEST_ASSERT_EQUAL_INT((int)PropertyType::Enum, (int)def.type);
    TEST_ASSERT_EQUAL_UINT8(0, def.min_value.as_enum());
    TEST_ASSERT_EQUAL_UINT8(5, def.max_value.as_enum());
    TEST_ASSERT_EQUAL_UINT8(1, def.default_value.as_enum());
}

// =============================================================================
// SimpleBeatShow: required_capabilities includes Mic and gates correctly.
// =============================================================================

static void test_simple_beat_required_capabilities(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    const auto req = sb->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::Mic));

    const auto host_with_mic = make_capability_mask(
        Capability::Mic, Capability::Display, Capability::AnalyserBeatDetection);
    TEST_ASSERT_TRUE(req.subset_of(host_with_mic));

    const auto host_no_mic = make_capability_mask(
        Capability::Display, Capability::AnalyserBeatDetection);
    TEST_ASSERT_FALSE(req.subset_of(host_no_mic));
}

// =============================================================================
// SimpleBeatShow: power profile - audio frames yes, spectrum / 8-band no.
// =============================================================================

static void test_simple_beat_power_profile(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    const auto p = sb->power();
    TEST_ASSERT_TRUE (p.needs_audio_frames);
    TEST_ASSERT_FALSE(p.needs_spectrum_frame);
    TEST_ASSERT_FALSE(p.needs_8band_summary);
}

// =============================================================================
// SimpleBeatShow: on_beat_detected fires the three render targets in
// the pre-Block-1 order (esp-now-broadcast, local DisplayClear, IR).
// =============================================================================

static void test_on_beat_fires_three_targets(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(1));   // Red - non-zero
    auto& ctx = sb->context();
    set_test_millis(100);
    sb->enter(ctx);

    set_test_millis(500);
    sb->on_beat_detected(ctx, 255);

    // 1. Wire (esp-now-broadcast): one RgbPulseEvent with Red rgb.
    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0xFF, g_espnow_driver.last_rgb_pulse().r);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_espnow_driver.last_rgb_pulse().g);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_espnow_driver.last_rgb_pulse().b);

    // 2. IR fires once: dispatch_output_class_group's Director-local
    // loopback for class 0 / 1 targets.
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0xFF, g_ir_driver.last_rgb_pulse().r);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_ir_driver.last_rgb_pulse().g);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_ir_driver.last_rgb_pulse().b);

    // 3. Screen flash fires DisplayClearEvent through LocalDriver,
    // which is registered by DAL::begin(); driver_send_count("local")
    // records it.
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1u,
        dal::DAL::driver_send_count("local"));
}

// =============================================================================
// SimpleBeatShow: paused state suppresses render fan-out (BPM tracking
// keeps updating so resume picks up cleanly).
// =============================================================================

static void test_on_beat_paused_skips_render(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(1));
    auto& ctx = sb->context();
    set_test_millis(100);
    sb->enter(ctx);

    ctx.set_paused(true);
    set_test_millis(500);
    sb->on_beat_detected(ctx, 255);

    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_INT(0, g_ir_driver.rgb_pulse_count());
}

// =============================================================================
// SimpleBeatShow: BPM tracking via IBI buffer; three beats at 500 ms
// spacing yields BPM=120 (60000/500).
// =============================================================================

static void test_bpm_tracking_from_ibi(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(1));
    auto& ctx = sb->context();
    set_test_millis(0);
    sb->enter(ctx);

    // Need at least 3 IBI samples for the median to populate. Fire 4
    // beats at 500 ms spacing.
    for (int i = 1; i <= 4; ++i) {
        set_test_millis(i * 500);
        sb->on_beat_detected(ctx, 255);
    }

    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, sb->estimated_bpm_for_tests());
}

// =============================================================================
// SimpleBeatShow: InputAction::Cycle advances colour 1 -> 2 -> ... -> 5 -> 0.
// =============================================================================

static void test_input_cycle_advances_colour(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(1));
    auto& ctx = sb->context();
    sb->enter(ctx);

    InputEvent ev{InputAction::Cycle, /*timestamp_ms=*/0};
    sb->on_input_action(ctx, ev);
    TEST_ASSERT_EQUAL_UINT8(2, bag.get("color").as_enum());

    bag.set("color", PropertyValue::from_enum(5));
    sb->on_input_action(ctx, ev);
    TEST_ASSERT_EQUAL_UINT8(0, bag.get("color").as_enum());   // wrap
}

// =============================================================================
// SimpleBeatShow: OFF colour (enum 0) sends an rgb=0 reset frame on
// each beat. The pre-Epic-4.7 behaviour gated IR on rgb != 0 so Off
// produced no IR fires; post-fix the dispatch loopback always fires
// when class matches, so a zero-rgb frame actively clears bracelet
// state. Off semantics shift from "no signal" to "actively turn off"
// - same visible result, cleaner in the face of residual state.
// =============================================================================

static void test_off_colour_fires_reset_pulse(void) {
    SimpleBeatShow* sb = simple_beat_show_instance();
    auto& bag = simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(0));   // Off
    auto& ctx = sb->context();
    set_test_millis(100);
    sb->enter(ctx);

    set_test_millis(500);
    sb->on_beat_detected(ctx, 255);

    // Wire fires a RgbPulse with zero rgb (Lumes' bindings decide
    // what to do with it).
    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0x00, g_espnow_driver.last_rgb_pulse().r);

    // IR fires too, with rgb=0 - bracelets receive the reset and go
    // to black. Same visible result as the old gated path, but with
    // any residual bracelet state actively cleared.
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT8(0x00, g_ir_driver.last_rgb_pulse().r);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_ir_driver.last_rgb_pulse().g);
    TEST_ASSERT_EQUAL_UINT8(0x00, g_ir_driver.last_rgb_pulse().b);
}

// =============================================================================
// Persistence migration: active_vis="beat-pulse" -> active_show="simple-beat".
// =============================================================================

static void test_migrate_active_vis_beat_pulse(void) {
    modes::persistence::test_seam::seed_legacy_active_vis("beat-pulse");
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_STRING("simple-beat",
        modes::persistence::load_active_show_id());
}

static void test_migrate_active_vis_spectrum_bars(void) {
    modes::persistence::test_seam::seed_legacy_active_vis("spectrum-bars");
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_STRING("simple-beat",
        modes::persistence::load_active_show_id());
}

static void test_migrate_active_vis_custom_id_pass_through(void) {
    modes::persistence::test_seam::seed_legacy_active_vis("custom-vis");
    modes::persistence::migrate_legacy_nvs_keys();
    TEST_ASSERT_EQUAL_STRING("custom-vis",
        modes::persistence::load_active_show_id());
}

static void test_migrate_active_vis_idempotent(void) {
    modes::persistence::test_seam::seed_legacy_active_vis("beat-pulse");
    modes::persistence::migrate_legacy_nvs_keys();
    modes::persistence::save_active_show_id("user-choice");
    modes::persistence::migrate_legacy_nvs_keys();   // second call no-op
    TEST_ASSERT_EQUAL_STRING("user-choice",
        modes::persistence::load_active_show_id());
}

// =============================================================================
// Unity main
// =============================================================================

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_show_kind_is_show);
    RUN_TEST(test_registry_register_find_clear);
    RUN_TEST(test_required_capabilities_can_be_outside_host);
    RUN_TEST(test_context_property_round_trip);
    RUN_TEST(test_simple_beat_identity);
    RUN_TEST(test_simple_beat_properties_schema);
    RUN_TEST(test_simple_beat_required_capabilities);
    RUN_TEST(test_simple_beat_power_profile);
    RUN_TEST(test_on_beat_fires_three_targets);
    RUN_TEST(test_on_beat_paused_skips_render);
    RUN_TEST(test_bpm_tracking_from_ibi);
    RUN_TEST(test_input_cycle_advances_colour);
    RUN_TEST(test_off_colour_fires_reset_pulse);
    RUN_TEST(test_migrate_active_vis_beat_pulse);
    RUN_TEST(test_migrate_active_vis_spectrum_bars);
    RUN_TEST(test_migrate_active_vis_custom_id_pass_through);
    RUN_TEST(test_migrate_active_vis_idempotent);
    return UNITY_END();
}
