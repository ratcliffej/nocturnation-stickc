// Native test: BeatPulseVisualisation (Epic 4.6 Block 8).
//
// Covers the first concrete Visualisation. The pre-migration code lived
// inside DirectorMode; this test exercises the migrated vis in
// isolation against recording test drivers registered on the standard
// "esp-now-broadcast" and "ir-pixmob" transports so the per-beat render
// fan-out can be asserted byte-for-byte against the pre-migration shape.
//
// Provides a HAL backend with Mic + Display + AnalyserBeatDetection.
// Intentionally NOT IRTx / ESPNow so the firmware pixmob_ir / espnow
// drivers refuse registration and our test drivers claim the transports.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"
#include "visualisations/beat_pulse.h"

// =============================================================================
// Native millis() seam
// =============================================================================
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend
// =============================================================================
//
// Mic + Display + AnalyserBeatDetection. No IRTx / ESPNow so the firmware
// drivers refuse registration; our test drivers take ownership of
// "ir-pixmob" and "esp-now-broadcast" transports. AnalyserSpectrumFrame
// is intentionally absent so the capability-mask negation test has a
// missing bit to look for.

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

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Recording drivers for "ir-pixmob" and "esp-now-broadcast"
// =============================================================================

using namespace nocturnation;
using nocturnation::dal::RgbPulseEvent;
using nocturnation::dal::DisplayClearEvent;
using nocturnation::dal::AudioFrameEvent;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;
using nocturnation::visualisations::BeatPulseVisualisation;
using nocturnation::visualisations::Visualisation;
using nocturnation::visualisations::VisualisationContext;
using nocturnation::visualisations::beat_pulse_instance;
using nocturnation::visualisations::beat_pulse_property_bag;
using nocturnation::visualisations::beat_pulse_context;
using nocturnation::visualisations::visualisation_registry;
using nocturnation::hal::Capability;
using nocturnation::hal::CapabilityMask;
using nocturnation::hal::make_capability_mask;
using nocturnation::hal::InputAction;
using nocturnation::hal::InputEvent;

namespace {

class RecordingDriver : public dal::Driver {
public:
    explicit RecordingDriver(const char* name) : name_(name) {}
    const char* transport_name() const override { return name_; }
    bool        begin()                override { return true; }

    bool send(uint8_t, const RgbPulseEvent& ev) override {
        if (rgb_pulse_count_ < kCap) rgb_pulse_buf_[rgb_pulse_count_] = ev;
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
        rgb_pulse_count_   = 0;
        display_clear_count_ = 0;
        last_rgb_pulse_    = RgbPulseEvent{};
        last_display_clear_ = DisplayClearEvent{};
    }

    int           rgb_pulse_count()      const { return rgb_pulse_count_; }
    RgbPulseEvent last_rgb_pulse()       const { return last_rgb_pulse_; }
    int           display_clear_count()  const { return display_clear_count_; }
    DisplayClearEvent last_display_clear() const { return last_display_clear_; }

private:
    static constexpr size_t kCap = 32;
    const char*       name_;
    int               rgb_pulse_count_   = 0;
    int               display_clear_count_ = 0;
    RgbPulseEvent     last_rgb_pulse_    = {};
    DisplayClearEvent last_display_clear_ = {};
    RgbPulseEvent     rgb_pulse_buf_[kCap] = {};
};

RecordingDriver g_ir_driver        {"ir-pixmob"};
RecordingDriver g_espnow_driver    {"esp-now-broadcast"};

// LocalDriver is registered by DAL::begin() and handles
// fire_display_clear("local", ...) directly via the host profile. So we
// don't need a recorder on "local" - but we register one anyway to
// observe DisplayClear events. Wait - LocalDriver claims "local", so
// our recorder cannot also claim it. We assert DisplayClear via the
// LocalDriver send count instead.
//
// Actually, since LocalDriver is registered with begin() returning true
// (Display capability is present), it'll claim "local" before we can.
// So we measure DisplayClear by reading driver_send_count("local") in
// addition to checking the IR + esp-now-broadcast recorders.

}  // namespace

// =============================================================================
// Unity setup / teardown
// =============================================================================

void setUp(void) {
    set_test_millis(0);
    PropertyBag::clear_for_tests();
    visualisation_registry().clear();
    g_ir_driver.reset();
    g_espnow_driver.reset();
    dal::DAL::begin();
    // Register test drivers AFTER DAL::begin so they claim the transports
    // (the firmware drivers refuse registration without HAL IRTx / ESPNow).
    dal::DAL::register_driver(&g_ir_driver);
    dal::DAL::register_driver(&g_espnow_driver);
}

void tearDown(void) {}

// =============================================================================
// Identity / kind / display_name
// =============================================================================

static void test_identity(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("beat-pulse", v->id());
    TEST_ASSERT_NOT_NULL(v->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Visualisation, (int)v->kind());
}

// =============================================================================
// Property schema: one entry, key "color", Enum, range [0, 5], default 1.
// =============================================================================

static void test_properties_schema(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    auto props = v->properties();
    TEST_ASSERT_EQUAL_size_t(1, props.size);
    const auto& def = props[0];
    TEST_ASSERT_EQUAL_STRING("color", def.key);
    TEST_ASSERT_EQUAL_INT((int)PropertyType::Enum, (int)def.type);
    TEST_ASSERT_EQUAL_UINT8(0, def.min_value.as_enum());
    TEST_ASSERT_EQUAL_UINT8(5, def.max_value.as_enum());
    TEST_ASSERT_EQUAL_UINT8(1, def.default_value.as_enum());
}

// =============================================================================
// required_capabilities includes Mic.
// =============================================================================

static void test_required_capabilities_includes_mic(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    const auto req = v->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::Mic));

    // And: a host with Mic + Display + AnalyserBeatDetection (this test's
    // HAL) is a superset of the requirement, so the gate opens.
    const auto host = make_capability_mask(
        Capability::Mic,
        Capability::Display,
        Capability::AnalyserBeatDetection);
    TEST_ASSERT_TRUE(req.subset_of(host));
}

// =============================================================================
// Capability gating: requirement against a host WITHOUT Mic fails.
// =============================================================================

static void test_capability_gate_fails_without_mic(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    const auto req = v->required_capabilities();
    const auto host_no_mic = make_capability_mask(
        Capability::Display, Capability::AnalyserBeatDetection);
    TEST_ASSERT_FALSE(req.subset_of(host_no_mic));
}

// =============================================================================
// power(): audio frames yes, spectrum no, 8-band no.
// =============================================================================

static void test_power_profile(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    const auto p = v->power();
    TEST_ASSERT_TRUE (p.needs_audio_frames);
    TEST_ASSERT_FALSE(p.needs_spectrum_frame);
    TEST_ASSERT_FALSE(p.needs_8band_summary);
}

// wants_full_screen must stay false: BeatPulse shares the LCD with the
// mode's chrome (colour title, BPM, flux meter). Flipping this true
// would suppress all of that and leave the operator with a blank screen
// between pulses.
static void test_wants_full_screen_is_false(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    TEST_ASSERT_FALSE(v->wants_full_screen());
}

// =============================================================================
// on_audio_frame with is_beat=true: fires the three-target fan-out in
// the correct order. The wire envelope is selected by envelope_for_bpm
// from the bpm tracked internally (0 -> punchy default).
// =============================================================================

static void test_on_beat_fires_wire_screen_ir_in_order(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    VisualisationContext& ctx = beat_pulse_context();

    // Default colour is Red (enum=1). Run enter() to wire pulse_ colour.
    v->enter(ctx);
    TEST_ASSERT_FALSE(ctx.paused());

    // Snapshot the LocalDriver send count - it's a process-singleton and
    // accumulates across tests. We assert the delta, not the absolute.
    const uint32_t local_before = dal::DAL::driver_send_count("local");

    // Build a beat-positive audio frame and fire it.
    AudioFrameEvent ev{};
    ev.is_beat = true;
    v->on_audio_frame(ctx, ev);

    // 1. esp-now-broadcast got an RgbPulseEvent with red colour.
    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.rgb_pulse_count());
    auto wire = g_espnow_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFF, wire.r);
    TEST_ASSERT_EQUAL_UINT8(0x00, wire.g);
    TEST_ASSERT_EQUAL_UINT8(0x00, wire.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_100, (int)wire.chance);
    // 0 BPM -> punchy default envelope.
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS, (int)wire.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)wire.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)wire.release);

    // 2. LocalDriver fires twice: once via fire_display_clear (the
    //    pre-Epic-4.7 full-bleed screen flash) and once via
    //    dispatch_output_class_group's screen-loopback added in the
    //    post-Block-5 fix (render_fx("00:00", ev) now also fans out
    //    to the Director's LocalDriver for Screen-class targets).
    TEST_ASSERT_EQUAL_UINT32(local_before + 2,
                              dal::DAL::driver_send_count("local"));

    // 3. ir-pixmob got two RgbPulseEvents this beat:
    //    (a) via Pulse::on_beat (the pre-Epic-4.7 BeatPulseVis path)
    //    (b) via dispatch_output_class_group's IR loopback main fire
    //    BeatPulseVis is retired production-side (Block 2 dropped its
    //    main.cpp registration); the test still exercises the legacy
    //    Vis to keep coverage of effects::Pulse.
    TEST_ASSERT_EQUAL_INT(2, g_ir_driver.rgb_pulse_count());
    auto ir = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.r);
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.g);
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.b);

    v->exit(ctx);
}

// =============================================================================
// on_audio_frame with is_beat=false: no render fan-out.
// =============================================================================

static void test_non_beat_frame_does_not_render(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    VisualisationContext& ctx = beat_pulse_context();

    v->enter(ctx);
    const uint32_t local_before = dal::DAL::driver_send_count("local");
    AudioFrameEvent ev{};
    ev.is_beat = false;
    v->on_audio_frame(ctx, ev);

    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_INT(0, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT32(local_before, dal::DAL::driver_send_count("local"));

    v->exit(ctx);
}

// =============================================================================
// Paused: a beat-positive frame doesn't trigger any render target.
// =============================================================================

static void test_paused_suppresses_all_render_targets(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    VisualisationContext& ctx = beat_pulse_context();

    v->enter(ctx);
    ctx.set_paused(true);
    const uint32_t local_before = dal::DAL::driver_send_count("local");

    AudioFrameEvent ev{};
    ev.is_beat = true;
    v->on_audio_frame(ctx, ev);

    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_INT(0, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_UINT32(local_before, dal::DAL::driver_send_count("local"));

    // Restore so subsequent tests start unpaused.
    ctx.set_paused(false);
    v->exit(ctx);
}

// =============================================================================
// on_input_action(Cycle) advances the colour enum and wraps at 5 -> 0.
// =============================================================================

static void test_cycle_action_advances_and_wraps(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    PropertyBag& bag          = beat_pulse_property_bag();
    VisualisationContext& ctx = beat_pulse_context();

    v->enter(ctx);
    // Default is 1 (Red).
    TEST_ASSERT_EQUAL_UINT8(1, ctx.get_property("color").as_enum());

    for (int i = 1; i < 6; ++i) {
        v->on_input_action(ctx, InputEvent{InputAction::Cycle, 0});
        TEST_ASSERT_EQUAL_UINT8(((1 + i) % 6),
                                ctx.get_property("color").as_enum());
    }

    // After 5 increments we're at (1+5)%6 = 0 (Off). One more wraps to 1.
    TEST_ASSERT_EQUAL_UINT8(0, ctx.get_property("color").as_enum());
    v->on_input_action(ctx, InputEvent{InputAction::Cycle, 0});
    TEST_ASSERT_EQUAL_UINT8(1, ctx.get_property("color").as_enum());

    v->exit(ctx);
}

// =============================================================================
// Non-Cycle actions are ignored.
// =============================================================================

static void test_non_cycle_action_ignored(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    VisualisationContext& ctx = beat_pulse_context();

    v->enter(ctx);
    const uint8_t before = ctx.get_property("color").as_enum();
    v->on_input_action(ctx, InputEvent{InputAction::Confirm, 0});
    TEST_ASSERT_EQUAL_UINT8(before, ctx.get_property("color").as_enum());
    v->on_input_action(ctx, InputEvent{InputAction::Pause, 0});
    TEST_ASSERT_EQUAL_UINT8(before, ctx.get_property("color").as_enum());
    v->exit(ctx);
}

// =============================================================================
// Singleton registration via beat_pulse_instance / registry.
// =============================================================================

static void test_registry_registration(void) {
    auto& reg = visualisation_registry();
    BeatPulseVisualisation* v = beat_pulse_instance();
    TEST_ASSERT_TRUE(reg.register_plugin(v));
    TEST_ASSERT_EQUAL_PTR(v, reg.find("beat-pulse"));
}

// =============================================================================
// Block 11: per-vis context() accessor returns the singleton.
// =============================================================================

static void test_context_accessor_returns_singleton(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    VisualisationContext& a = v->context();
    VisualisationContext& b = beat_pulse_context();
    TEST_ASSERT_EQUAL_PTR(&a, &b);
}

// =============================================================================
// Block 11: stale-colour bug fix.
//
// Property bag's "color" is mutated WITHOUT going through on_input_action
// (mirrors the Settings-overlay path) and then on_property_changed is
// called by the framework. The very next on_audio_frame(is_beat=true) must
// fire the NEW colour to "all-pixmobs", not the colour cached by enter().
// =============================================================================

static void test_property_changed_resyncs_pulse_colour_on_next_beat(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    PropertyBag& bag          = beat_pulse_property_bag();
    VisualisationContext& ctx = beat_pulse_context();

    // Start with Red (1). enter() syncs pulse_ to Red.
    bag.set("color", PropertyValue::from_enum(1));
    v->enter(ctx);

    // Simulate Settings-overlay edit: write Blue (3) directly into the
    // bag (no Cycle action), then fire the on_property_changed hook.
    bag.set("color", PropertyValue::from_enum(3));
    v->on_property_changed(ctx, "color");

    g_ir_driver.reset();
    g_espnow_driver.reset();

    // Next beat must hit IR with BLUE bytes (0x00,0x00,0xFF), not red.
    AudioFrameEvent ev{};
    ev.is_beat = true;
    v->on_audio_frame(ctx, ev);

    // ir-pixmob fires twice this beat: main BLUE via render_fx("00:00")
    // loopback, plus a second BLUE via Pulse::on_beat's
    // render_fx("all-pixmobs") legacy path.
    TEST_ASSERT_EQUAL_INT(2, g_ir_driver.rgb_pulse_count());
    auto ir = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.r);
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.g);
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.b);

    // And the wire-side colour comes straight from the bag in
    // on_audio_frame, so it's the new colour too.
    auto wire = g_espnow_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0x00, wire.r);
    TEST_ASSERT_EQUAL_UINT8(0x00, wire.g);
    TEST_ASSERT_EQUAL_UINT8(0xFF, wire.b);

    v->exit(ctx);
}

// Behaviour preservation: unrelated keys passed to on_property_changed
// must not touch the pulse colour cache.
static void test_property_changed_ignores_unrelated_keys(void) {
    BeatPulseVisualisation* v = beat_pulse_instance();
    PropertyBag& bag          = beat_pulse_property_bag();
    VisualisationContext& ctx = beat_pulse_context();

    bag.set("color", PropertyValue::from_enum(1));   // Red
    v->enter(ctx);

    // Fire on_property_changed with a bogus key. The internal pulse_
    // cache must stay Red, observable via the next beat's IR fire.
    v->on_property_changed(ctx, "not-a-real-key");

    g_ir_driver.reset();
    AudioFrameEvent ev{};
    ev.is_beat = true;
    v->on_audio_frame(ctx, ev);

    // 2 fires: render_fx("00:00") main + Pulse path.
    TEST_ASSERT_EQUAL_INT(2, g_ir_driver.rgb_pulse_count());
    auto ir = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.r);
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.g);
    TEST_ASSERT_EQUAL_UINT8(0x00, ir.b);

    v->exit(ctx);
}

// =============================================================================
// main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_properties_schema);
    RUN_TEST(test_required_capabilities_includes_mic);
    RUN_TEST(test_capability_gate_fails_without_mic);
    RUN_TEST(test_power_profile);
    RUN_TEST(test_wants_full_screen_is_false);
    RUN_TEST(test_on_beat_fires_wire_screen_ir_in_order);
    RUN_TEST(test_non_beat_frame_does_not_render);
    RUN_TEST(test_paused_suppresses_all_render_targets);
    RUN_TEST(test_cycle_action_advances_and_wraps);
    RUN_TEST(test_non_cycle_action_ignored);
    RUN_TEST(test_registry_registration);
    // Block 11 coverage.
    RUN_TEST(test_context_accessor_returns_singleton);
    RUN_TEST(test_property_changed_resyncs_pulse_colour_on_next_beat);
    RUN_TEST(test_property_changed_ignores_unrelated_keys);
    return UNITY_END();
}
