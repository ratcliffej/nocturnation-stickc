// Native test: SpectrumBarsVisualisation (Epic 4.6 Block 11).
//
// Mirrors the structure of test_beat_pulse: a tiny HAL backend that
// declares Mic + Display + AnalyserSpectrumFrame so SpectrumBars'
// capability gate opens AND the DAL composes a SpectrumFrame input on
// the "local" target. Recording drivers stand in for IR + ESP-NOW so
// the manual-fire fan-out can be byte-asserted.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"
#include "visualisations/spectrum_bars.h"

// =============================================================================
// Native millis() seam
// =============================================================================
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
static void set_test_millis(uint32_t v) { s_native_millis = v; }

// =============================================================================
// Test HAL backend - Mic + Display + AnalyserSpectrumFrame.
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
    Capability::AnalyserSpectrumFrame,
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
// Recording drivers for "ir-pixmob" and "esp-now-broadcast"
// =============================================================================

using namespace nocturnation;
using dal::RgbPulseEvent;
using dal::DisplayClearEvent;
using dal::DisplayFillRectEvent;
using dal::AudioFrameEvent;
using dal::SpectrumFrameEvent;
using plugins::PluginKind;
using plugins::PropertyBag;
using plugins::PropertyDef;
using plugins::PropertyType;
using plugins::PropertyValue;
using plugins::Span;
using visualisations::SpectrumBarsVisualisation;
using visualisations::VisualisationContext;
using visualisations::spectrum_bars_instance;
using visualisations::spectrum_bars_property_bag;
using visualisations::spectrum_bars_context;
using visualisations::visualisation_registry;
using hal::Capability;
using hal::InputAction;
using hal::InputEvent;
using hal::make_capability_mask;

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
    bool send(uint8_t, const DisplayFillRectEvent& ev) override {
        ++fill_rect_count_;
        last_fill_rect_ = ev;
        return true;
    }

    void reset() {
        rgb_pulse_count_      = 0;
        display_clear_count_  = 0;
        fill_rect_count_      = 0;
        last_rgb_pulse_       = RgbPulseEvent{};
        last_display_clear_   = DisplayClearEvent{};
        last_fill_rect_       = DisplayFillRectEvent{};
    }

    int               rgb_pulse_count()     const { return rgb_pulse_count_; }
    RgbPulseEvent     last_rgb_pulse()      const { return last_rgb_pulse_; }
    int               display_clear_count() const { return display_clear_count_; }
    DisplayClearEvent last_display_clear()  const { return last_display_clear_; }
    int               fill_rect_count()     const { return fill_rect_count_; }
    DisplayFillRectEvent last_fill_rect()   const { return last_fill_rect_; }

private:
    const char* name_;
    int rgb_pulse_count_     = 0;
    int display_clear_count_ = 0;
    int fill_rect_count_     = 0;
    RgbPulseEvent        last_rgb_pulse_     = {};
    DisplayClearEvent    last_display_clear_ = {};
    DisplayFillRectEvent last_fill_rect_     = {};
};

RecordingDriver g_ir_driver        {"ir-pixmob"};
RecordingDriver g_espnow_driver    {"esp-now-broadcast"};

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
    // Register test drivers AFTER DAL::begin so they claim the
    // transports the firmware drivers refused for lack of IRTx / ESPNow.
    dal::DAL::register_driver(&g_ir_driver);
    dal::DAL::register_driver(&g_espnow_driver);
}

void tearDown(void) {}

// =============================================================================
// Identity / display name / kind / id-length sanity
// =============================================================================

static void test_identity(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("spec-bars", v->id());
    TEST_ASSERT_EQUAL_STRING("Spectrum Bars", v->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Visualisation, (int)v->kind());

    // NVS namespace "nv_<id>" must fit in 15 chars (ESP-IDF Preferences
    // limit). "nv_spec-bars" = 12 chars, comfortably under.
    TEST_ASSERT_LESS_OR_EQUAL_size_t(12, std::strlen(v->id()));
}

// =============================================================================
// Property schema: sensitivity only. band_focus was dropped when the vis
// moved to 7 fixed-colour perceptual bands.
// =============================================================================

static void test_properties_schema(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    auto props = v->properties();
    TEST_ASSERT_EQUAL_size_t(1, props.size);

    const auto& def0 = props[0];
    TEST_ASSERT_EQUAL_STRING("sensitivity", def0.key);
    TEST_ASSERT_EQUAL_INT((int)PropertyType::U8, (int)def0.type);
    TEST_ASSERT_EQUAL_UINT8(1,  def0.min_value.as_u8());
    TEST_ASSERT_EQUAL_UINT8(10, def0.max_value.as_u8());
    TEST_ASSERT_EQUAL_UINT8(5,  def0.default_value.as_u8());
    TEST_ASSERT_NULL(def0.unit);
}

// =============================================================================
// required_capabilities: Mic. Capability gate opens against test HAL.
// =============================================================================

static void test_capability_gate_open_against_test_host(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    const auto req = v->required_capabilities();
    TEST_ASSERT_TRUE(req.has(Capability::Mic));

    const auto host = make_capability_mask(
        Capability::Mic,
        Capability::Display,
        Capability::AnalyserSpectrumFrame);
    TEST_ASSERT_TRUE(req.subset_of(host));
}

// =============================================================================
// Power profile: needs_audio_frames + needs_spectrum_frame, 30 Hz LCD cap.
// =============================================================================

static void test_power_profile(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    const auto p = v->power();
    TEST_ASSERT_TRUE (p.needs_audio_frames);
    TEST_ASSERT_TRUE (p.needs_spectrum_frame);
    TEST_ASSERT_FALSE(p.needs_8band_summary);
    TEST_ASSERT_EQUAL_UINT16(30, p.lcd_refresh_hz_max);
    TEST_ASSERT_EQUAL_UINT16(0,  p.tick_hz);
}

// wants_full_screen must stay true: SpectrumBars paints the entire
// LCD itself and the mode's 50 ms chrome draw would otherwise clobber
// the bars (see hotfix 2026-05-11).
static void test_wants_full_screen_is_true(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    TEST_ASSERT_TRUE(v->wants_full_screen());
}

// =============================================================================
// context() returns the singleton (Block 11 generalisation).
// =============================================================================

static void test_context_accessor_returns_singleton(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& a = v->context();
    VisualisationContext& b = spectrum_bars_context();
    TEST_ASSERT_EQUAL_PTR(&a, &b);
}

// =============================================================================
// on_spectrum_frame: draws bars (DisplayFillRect events on "local").
// =============================================================================

static void test_on_spectrum_frame_draws_bars(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);

    // Feed magnitudes that should produce non-zero bar heights.
    SpectrumFrameEvent ev{};
    for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
        ev.magnitudes[i] = 50000.0f;  // representative loud-music value
    }

    const uint32_t local_before = dal::DAL::driver_send_count("local");
    set_test_millis(100);
    v->on_spectrum_frame(ctx, ev);

    // Each frame produces at least 1 clear + N bars (some may be zero
    // height and skipped). With magnitude 15 and default sensitivity 5
    // the bars are non-zero, so we expect >= kBands fill_rects in addition
    // to the clear.
    const uint32_t local_after = dal::DAL::driver_send_count("local");
    TEST_ASSERT_GREATER_THAN_UINT32(local_before, local_after);

    v->exit(ctx);
}

static void test_on_spectrum_frame_paused_suppresses_render(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);
    ctx.set_paused(true);

    SpectrumFrameEvent ev{};
    for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
        ev.magnitudes[i] = 50000.0f;
    }

    const uint32_t local_before = dal::DAL::driver_send_count("local");
    set_test_millis(100);
    v->on_spectrum_frame(ctx, ev);
    TEST_ASSERT_EQUAL_UINT32(local_before, dal::DAL::driver_send_count("local"));

    ctx.set_paused(false);
    v->exit(ctx);
}

// 30 Hz throttle: a second on_spectrum_frame within 33 ms must not redraw.
static void test_on_spectrum_frame_throttles_to_30hz(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);

    SpectrumFrameEvent ev{};
    for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
        ev.magnitudes[i] = 50000.0f;
    }

    set_test_millis(1000);
    v->on_spectrum_frame(ctx, ev);
    const uint32_t after_first = dal::DAL::driver_send_count("local");

    // 10 ms later: must not produce any new sends.
    set_test_millis(1010);
    v->on_spectrum_frame(ctx, ev);
    TEST_ASSERT_EQUAL_UINT32(after_first, dal::DAL::driver_send_count("local"));

    // 33 ms later: redraws.
    set_test_millis(1033);
    v->on_spectrum_frame(ctx, ev);
    TEST_ASSERT_GREATER_THAN_UINT32(after_first,
                                    dal::DAL::driver_send_count("local"));

    v->exit(ctx);
}

// =============================================================================
// Confirm fires the three-target manual beat at CHANCE_100 in white -
// no per-band tinting any more (band_focus was dropped with the
// perceptual-band rewrite).
// =============================================================================

static void test_confirm_fires_manual_beat_white(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);

    g_ir_driver.reset();
    g_espnow_driver.reset();

    v->on_input_action(ctx, InputEvent{InputAction::Confirm, 0});

    // 1. wire (esp-now-broadcast) RgbPulseEvent: white + CHANCE_100.
    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.rgb_pulse_count());
    auto wire = g_espnow_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFF, wire.r);
    TEST_ASSERT_EQUAL_UINT8(0xFF, wire.g);
    TEST_ASSERT_EQUAL_UINT8(0xFF, wire.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_100, (int)wire.chance);

    // 2. ir-pixmob fires twice this beat: once via SpectrumBarsVis's
    //    explicit all-pixmobs call, once via dispatch_output_class_group's
    //    master-IR loopback added post-Block-5 (render_fx("00:00", ev)
    //    now reaches the master's IR alongside the ESP-NOW broadcast).
    //    Both fires carry the same WHITE bytes.
    TEST_ASSERT_EQUAL_INT(2, g_ir_driver.rgb_pulse_count());
    auto ir = g_ir_driver.last_rgb_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.r);
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.g);
    TEST_ASSERT_EQUAL_UINT8(0xFF, ir.b);

    v->exit(ctx);
}

static void test_confirm_paused_does_not_fire(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);
    ctx.set_paused(true);

    g_ir_driver.reset();
    g_espnow_driver.reset();

    v->on_input_action(ctx, InputEvent{InputAction::Confirm, 0});

    TEST_ASSERT_EQUAL_INT(0, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.rgb_pulse_count());

    ctx.set_paused(false);
    v->exit(ctx);
}

static void test_non_confirm_actions_do_not_fire(void) {
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    VisualisationContext& ctx    = spectrum_bars_context();
    v->enter(ctx);

    g_ir_driver.reset();
    g_espnow_driver.reset();

    v->on_input_action(ctx, InputEvent{InputAction::Cycle, 0});
    v->on_input_action(ctx, InputEvent{InputAction::CyclePrev, 0});
    v->on_input_action(ctx, InputEvent{InputAction::Pause, 0});

    TEST_ASSERT_EQUAL_INT(0, g_ir_driver.rgb_pulse_count());
    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.rgb_pulse_count());

    v->exit(ctx);
}

// =============================================================================
// Registry: SpectrumBars registers as a Visualisation with id "spec-bars".
// =============================================================================

static void test_registry_registration(void) {
    auto& reg = visualisation_registry();
    SpectrumBarsVisualisation* v = spectrum_bars_instance();
    TEST_ASSERT_TRUE(reg.register_plugin(v));
    TEST_ASSERT_EQUAL_PTR(v, reg.find("spec-bars"));
}

// =============================================================================
// main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_properties_schema);
    RUN_TEST(test_capability_gate_open_against_test_host);
    RUN_TEST(test_power_profile);
    RUN_TEST(test_wants_full_screen_is_true);
    RUN_TEST(test_context_accessor_returns_singleton);
    RUN_TEST(test_on_spectrum_frame_draws_bars);
    RUN_TEST(test_on_spectrum_frame_paused_suppresses_render);
    RUN_TEST(test_on_spectrum_frame_throttles_to_30hz);
    RUN_TEST(test_confirm_fires_manual_beat_white);
    RUN_TEST(test_confirm_paused_does_not_fire);
    RUN_TEST(test_non_confirm_actions_do_not_fire);
    RUN_TEST(test_registry_registration);
    return UNITY_END();
}
