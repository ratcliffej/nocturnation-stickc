// Native test: Visualisation contract (Epic 4.6 Block 5).
//
// Covers the interface + context + registry surface that concrete
// visualisations will land on in Blocks 8 and 11. No concrete vis
// here - just a tiny StubVisualisation in this TU that records every
// hook invocation so we can verify routing.
//
// Provides a miniature HAL backend so DAL::begin() composes a host
// profile that lets render_fx("local", RgbPulseEvent{...}) succeed
// (Display capability -> RgbPulse output on the local profile -> the
// LocalDriver registered by DAL::begin handles dispatch).

#include <unity.h>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"

// =============================================================================
// Native millis() seam (mirrors mode_machine.cpp's pattern)
// =============================================================================
//
// VisualisationContext::since_enter_ms() reads millis() through the
// shared shim in src/visualisations/visualisation_context.cpp. The TU
// there provides a weak fallback that returns 0; this strong
// definition wins at link time, mirroring the seam mode_machine.cpp
// installs on native builds in the other env mixes. Tests advance the
// clock via set_test_millis() so since_enter_ms() arithmetic is
// observable.
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }

static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend
// =============================================================================
//
// Declares Mic + Display + AnalyserBeatDetection + AnalyserBandSummary
// (intentionally NOT AnalyserSpectrumFrame or AnalyserDropDetection, so
// analyser_caps() returns a strict subset of the four-bit space and we
// can check both presence and absence).

namespace nocturnation {
namespace hal {

// Minimal Display stub. Every method is a no-op; LocalDriver's
// RgbPulseEvent send path requires hal::HAL::display() != nullptr to
// succeed, so we provide an instance whose draw calls go to /dev/null.
// This lets the render_fx("local", ...) dispatch path return true so
// the test can verify routing through the context.
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
// StubVisualisation
// =============================================================================
//
// Records every hook invocation. on_audio_frame fires a
// render_fx("local", ev) so we can verify forwarding.

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
using nocturnation::plugins::Span;
using nocturnation::visualisations::Visualisation;
using nocturnation::visualisations::VisualisationContext;
using nocturnation::visualisations::VisualisationRegistry;
using nocturnation::visualisations::visualisation_registry;

namespace {

const PropertyDef kStubSchema[] = {
    {
        /*key=*/"level",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(50),
        /*min_value=*/PropertyValue::from_u8(0),
        /*max_value=*/PropertyValue::from_u8(100),
        /*display_name=*/"Level",
        /*unit=*/"%",
        /*enum_names=*/nullptr,
    },
};

class StubVisualisation : public Visualisation {
public:
    const char* id()           const override { return "stub-vis"; }
    const char* display_name() const override { return "Stub"; }

    Span<const PropertyDef> properties() const override {
        return Span<const PropertyDef>(kStubSchema,
                                       sizeof(kStubSchema) / sizeof(kStubSchema[0]));
    }

    void enter(VisualisationContext&)                                              override { ++enter_calls; }
    void exit (VisualisationContext&)                                              override { ++exit_calls;  }
    void on_audio_frame   (VisualisationContext& ctx,
                            const dal::AudioFrameEvent&)                            override {
        ++audio_calls;
        dal::RgbPulseEvent ev{255, 64, 32,
                              pixmob::T_32_MS, pixmob::T_96_MS,
                              pixmob::T_96_MS, pixmob::CHANCE_100};
        last_render_ok = ctx.render_fx("local", ev);
    }
    void on_spectrum_frame(VisualisationContext&,
                            const dal::SpectrumFrameEvent&)                         override { ++spectrum_calls; }
    void on_input_action  (VisualisationContext&, const hal::InputEvent&)          override { ++input_calls;    }
    void tick             (VisualisationContext&, uint32_t now)                    override { ++tick_calls; last_tick_ms = now; }
    // Block 11 added a pure-virtual context() accessor on Visualisation.
    // This test stub creates its own bag/context on-the-fly elsewhere;
    // the accessor returns a function-static singleton that's never
    // actually exercised by the cases below but exists so the class
    // remains instantiable.
    VisualisationContext& context() override {
        static PropertyBag bag(*this);
        static VisualisationContext c(*this, bag);
        return c;
    }

    void reset() {
        enter_calls = exit_calls = audio_calls = spectrum_calls = input_calls = tick_calls = 0;
        last_tick_ms = 0;
        last_render_ok = false;
    }

    int      enter_calls    = 0;
    int      exit_calls     = 0;
    int      audio_calls    = 0;
    int      spectrum_calls = 0;
    int      input_calls    = 0;
    int      tick_calls     = 0;
    uint32_t last_tick_ms   = 0;
    bool     last_render_ok = false;
};

// A vis with no schema and a requirement the test HAL DOESN'T have,
// for the gating test below.
class DemandingVis : public Visualisation {
public:
    const char* id()           const override { return "demanding"; }
    const char* display_name() const override { return "Demanding"; }
    CapabilityMask required_capabilities() const override {
        return make_capability_mask(Capability::AnalyserSpectrumFrame);
    }
    VisualisationContext& context() override {
        static PropertyBag bag(*this);
        static VisualisationContext c(*this, bag);
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
    visualisation_registry().clear();
    dal::DAL::begin();
}

void tearDown(void) {}

// =============================================================================
// Visualisation::kind() is locked to PluginKind::Visualisation
// =============================================================================

static void test_visualisation_kind_is_visualisation(void) {
    StubVisualisation v;
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Visualisation, (int)v.kind());

    DemandingVis d;
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Visualisation, (int)d.kind());
}

// =============================================================================
// Registry singleton: register / find / clear via visualisation_registry()
// =============================================================================

static void test_registry_register_find_clear(void) {
    StubVisualisation v;
    auto& reg = visualisation_registry();

    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_TRUE(reg.register_plugin(&v));
    TEST_ASSERT_EQUAL_size_t(1, reg.count());
    TEST_ASSERT_EQUAL_PTR(&v, reg.find("stub-vis"));
    TEST_ASSERT_NULL(reg.find("nope"));

    // Singleton: a second call returns the same registry, so the
    // registration is still visible.
    TEST_ASSERT_EQUAL_PTR(&v, visualisation_registry().find("stub-vis"));

    reg.clear();
    TEST_ASSERT_EQUAL_size_t(0, reg.count());
    TEST_ASSERT_NULL(reg.find("stub-vis"));
}

// =============================================================================
// Capability gating: vis with unmet requirements can still register, but
// required.subset_of(host) returns false so the framework can gate selection.
// =============================================================================

static void test_required_capabilities_can_be_outside_host_capabilities(void) {
    DemandingVis demanding;
    auto& reg = visualisation_registry();
    TEST_ASSERT_TRUE(reg.register_plugin(&demanding));

    // Host (test HAL) declares Beat + BandSummary, not SpectrumFrame.
    auto host = make_capability_mask(
        Capability::AnalyserBeatDetection,
        Capability::AnalyserBandSummary);
    TEST_ASSERT_FALSE(demanding.required_capabilities().subset_of(host));

    // A vis with NO required caps is trivially a subset of any host mask.
    StubVisualisation stub;
    TEST_ASSERT_TRUE(stub.required_capabilities().subset_of(host));
}

// =============================================================================
// VisualisationContext: property bag round-trip + clamping
// =============================================================================

static void test_context_property_default_then_set_get(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    // Never-set: returns the schema default (50).
    TEST_ASSERT_EQUAL_UINT8(50, ctx.get_property("level").as_u8());

    // Set then read back.
    TEST_ASSERT_TRUE(ctx.set_property("level", PropertyValue::from_u8(75)));
    TEST_ASSERT_EQUAL_UINT8(75, ctx.get_property("level").as_u8());
}

static void test_context_property_set_clamps_out_of_range(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    // Out of range above max: clamps to 100.
    TEST_ASSERT_TRUE(ctx.set_property("level", PropertyValue::from_u8(200)));
    TEST_ASSERT_EQUAL_UINT8(100, ctx.get_property("level").as_u8());

    // For U8 with min 0, "below min" can't be expressed (0 is the floor of
    // the type), so this leg is implicitly covered by the type itself.
    // Verify clamping at the upper edge holds across a refresh of the bag.
    TEST_ASSERT_TRUE(ctx.set_property("level", PropertyValue::from_u8(255)));
    TEST_ASSERT_EQUAL_UINT8(100, ctx.get_property("level").as_u8());
}

// =============================================================================
// VisualisationContext: pause flag
// =============================================================================

static void test_context_paused_round_trip(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    TEST_ASSERT_FALSE(ctx.paused());
    ctx.set_paused(true);
    TEST_ASSERT_TRUE(ctx.paused());
    ctx.set_paused(false);
    TEST_ASSERT_FALSE(ctx.paused());
}

// =============================================================================
// VisualisationContext: identity accessors
// =============================================================================

static void test_context_identity_accessors(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    TEST_ASSERT_EQUAL_PTR(&v,   &ctx.visualisation());
    TEST_ASSERT_EQUAL_PTR(&bag, &ctx.property_bag());
}

// =============================================================================
// VisualisationContext: time arithmetic via mark_entered + since_enter_ms
// =============================================================================

static void test_context_since_enter_ms_after_mark_entered(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    set_test_millis(1000);
    ctx.mark_entered(1000);
    TEST_ASSERT_EQUAL_UINT32(1000u, ctx.now_ms());
    TEST_ASSERT_EQUAL_UINT32(0u,    ctx.since_enter_ms());

    set_test_millis(1750);
    TEST_ASSERT_EQUAL_UINT32(1750u, ctx.now_ms());
    TEST_ASSERT_EQUAL_UINT32(750u,  ctx.since_enter_ms());
}

// =============================================================================
// VisualisationContext: analyser_caps reflects HAL declarations
// =============================================================================

static void test_context_analyser_caps_reflects_hal(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    const auto caps = ctx.analyser_caps();
    TEST_ASSERT_TRUE (caps.has(Capability::AnalyserBeatDetection));
    TEST_ASSERT_TRUE (caps.has(Capability::AnalyserBandSummary));
    TEST_ASSERT_FALSE(caps.has(Capability::AnalyserSpectrumFrame));
    TEST_ASSERT_FALSE(caps.has(Capability::AnalyserDropDetection));
}

// =============================================================================
// render_fx routing through the context
// =============================================================================
//
// "local" is registered as a device by DAL::begin() and the test HAL
// declares Display, so the host profile carries RgbPulse as an output.
// The LocalDriver is registered by DAL::begin() too; it accepts the
// dispatch (returns true) even when hal::HAL::display() is null - the
// driver's send(RgbPulseEvent) path sets up the pulse-animation state
// and the actual paint goes through hal::HAL::display() later, which
// is null in this test (so nothing visible happens) but the send()
// override still returns true.
//
// "nonexistent" has no registered device; dispatch_output returns false
// at the find_device step.

static void test_context_render_fx_local_succeeds(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    dal::RgbPulseEvent ev{255, 0, 0,
                          pixmob::T_32_MS, pixmob::T_96_MS,
                          pixmob::T_96_MS, pixmob::CHANCE_100};
    TEST_ASSERT_TRUE(ctx.render_fx("local", ev));
}

static void test_context_render_fx_unknown_target_fails(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    dal::RgbPulseEvent ev{};
    TEST_ASSERT_FALSE(ctx.render_fx("nonexistent-target", ev));
}

// =============================================================================
// Stub hooks: enter / exit / on_audio_frame / on_spectrum_frame /
// on_input_action / tick all dispatch through cleanly.
// =============================================================================

static void test_stub_hooks_recorded(void) {
    StubVisualisation v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    v.enter(ctx);
    TEST_ASSERT_EQUAL_INT(1, v.enter_calls);

    dal::AudioFrameEvent af{};
    v.on_audio_frame(ctx, af);
    TEST_ASSERT_EQUAL_INT(1, v.audio_calls);
    // The stub fires render_fx("local", ...) inside on_audio_frame and
    // records the return; "local" is registered, so it should be true.
    TEST_ASSERT_TRUE(v.last_render_ok);

    dal::SpectrumFrameEvent sf{};
    v.on_spectrum_frame(ctx, sf);
    TEST_ASSERT_EQUAL_INT(1, v.spectrum_calls);

    hal::InputEvent ie{hal::InputAction::Confirm, 42};
    v.on_input_action(ctx, ie);
    TEST_ASSERT_EQUAL_INT(1, v.input_calls);

    v.tick(ctx, 12345);
    TEST_ASSERT_EQUAL_INT(1, v.tick_calls);
    TEST_ASSERT_EQUAL_UINT32(12345u, v.last_tick_ms);

    v.exit(ctx);
    TEST_ASSERT_EQUAL_INT(1, v.exit_calls);
}

// =============================================================================
// Default Visualisation hooks are no-ops (don't crash, don't side-effect).
// =============================================================================

namespace {
class BareVis : public Visualisation {
public:
    const char* id()           const override { return "bare"; }
    const char* display_name() const override { return "Bare"; }
    VisualisationContext& context() override {
        static PropertyBag bag(*this);
        static VisualisationContext c(*this, bag);
        return c;
    }
};
}  // namespace

static void test_default_hooks_are_no_ops(void) {
    BareVis v;
    PropertyBag bag(v);
    VisualisationContext ctx(v, bag);

    // None of these should crash. There's nothing to assert beyond
    // "doesn't blow up"; the value here is that the default override
    // surface compiles and links.
    v.enter(ctx);
    v.exit(ctx);
    dal::AudioFrameEvent af{};
    v.on_audio_frame(ctx, af);
    dal::SpectrumFrameEvent sf{};
    v.on_spectrum_frame(ctx, sf);
    hal::InputEvent ie{};
    v.on_input_action(ctx, ie);
    v.tick(ctx, 0);

    TEST_ASSERT_EQUAL_INT((int)PluginKind::Visualisation, (int)v.kind());
}

// =============================================================================
// Power profile default for a Visualisation matches the Plugin default.
// =============================================================================

static void test_visualisation_default_power_profile(void) {
    BareVis v;
    auto p = v.power();
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
    RUN_TEST(test_visualisation_kind_is_visualisation);
    RUN_TEST(test_registry_register_find_clear);
    RUN_TEST(test_required_capabilities_can_be_outside_host_capabilities);
    RUN_TEST(test_context_property_default_then_set_get);
    RUN_TEST(test_context_property_set_clamps_out_of_range);
    RUN_TEST(test_context_paused_round_trip);
    RUN_TEST(test_context_identity_accessors);
    RUN_TEST(test_context_since_enter_ms_after_mark_entered);
    RUN_TEST(test_context_analyser_caps_reflects_hal);
    RUN_TEST(test_context_render_fx_local_succeeds);
    RUN_TEST(test_context_render_fx_unknown_target_fails);
    RUN_TEST(test_stub_hooks_recorded);
    RUN_TEST(test_default_hooks_are_no_ops);
    RUN_TEST(test_visualisation_default_power_profile);
    return UNITY_END();
}
