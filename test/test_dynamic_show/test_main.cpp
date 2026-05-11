// Native test: DynamicShow (Epic 4.7 Block 5).
//
// Exercises the headline Show against recording drivers registered on
// the standard "esp-now-broadcast" + "ir-pixmob" transports. The
// class+group routing dispatches to esp-now-broadcast for "01:0X"
// targets, so the test driver records the per-event group and the
// RgbPulseEvent payload (rgb + envelope + chance) for assertion.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "dal/analyser/section_detector.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "shows/show.h"
#include "shows/show_context.h"
#include "shows/show_registry.h"
#include "shows/dynamic_show.h"

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
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Recording driver - captures the per-event group and RgbPulseEvent.
// =============================================================================

using namespace nocturnation;
using nocturnation::dal::RgbPulseEvent;
using nocturnation::dal::DisplayClearEvent;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyValue;
using nocturnation::shows::DynamicShow;
using nocturnation::shows::ShowContext;
using nocturnation::shows::dynamic_show_context;
using nocturnation::shows::dynamic_show_instance;
using nocturnation::shows::dynamic_show_property_bag;
using nocturnation::shows::show_registry;

namespace {

struct Captured {
    uint8_t       target_group;
    RgbPulseEvent event;
};

class RecordingDriver : public dal::Driver {
public:
    explicit RecordingDriver(const char* name) : name_(name) {}
    const char* transport_name() const override { return name_; }
    bool        begin()                override { return true; }

    bool send(uint8_t target_group, const RgbPulseEvent& ev) override {
        if (count_ < kCap) {
            buf_[count_] = Captured{target_group, ev};
            ++count_;
        }
        last_group_ = target_group;
        last_event_ = ev;
        return true;
    }
    bool send(uint8_t, const DisplayClearEvent&) override {
        return true;
    }

    void reset() {
        count_ = 0;
        last_group_ = 0;
        last_event_ = RgbPulseEvent{};
    }

    int           count()       const { return count_; }
    uint8_t       last_group()  const { return last_group_; }
    RgbPulseEvent last_event()  const { return last_event_; }
    Captured      at(size_t i)  const { return buf_[i]; }

private:
    static constexpr size_t kCap = 32;
    const char*       name_;
    int               count_       = 0;
    uint8_t           last_group_  = 0;
    RgbPulseEvent     last_event_  = {};
    Captured          buf_[kCap]   = {};
};

RecordingDriver g_ir_driver    {"ir-pixmob"};
RecordingDriver g_espnow_driver{"esp-now-broadcast"};

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
    dynamic_show_context().set_paused(false);
    // Reset internal state by re-entering the show.
    dynamic_show_instance()->enter(dynamic_show_context());
    dal::DAL::begin();
    dal::DAL::register_driver(&g_ir_driver);
    dal::DAL::register_driver(&g_espnow_driver);
}

void tearDown(void) {}

// =============================================================================
// Identity
// =============================================================================

static void test_identity(void) {
    DynamicShow* d = dynamic_show_instance();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_STRING("dynamic", d->id());
    TEST_ASSERT_EQUAL_STRING("Dynamic", d->display_name());
    TEST_ASSERT_EQUAL_INT((int)PluginKind::Show, (int)d->kind());
}

static void test_required_capabilities_includes_mic(void) {
    DynamicShow* d = dynamic_show_instance();
    TEST_ASSERT_TRUE(d->required_capabilities().has(hal::Capability::Mic));
}

// =============================================================================
// Routing: kick -> group 1, snare -> group 2, hi-hat -> group 3.
// =============================================================================

// All-events-to-group-0 routing is the new default (groups=1). Tests
// that exercise per-drum group routing must explicitly opt into the
// 3-group config first.
static void set_groups(uint8_t n) {
    dynamic_show_property_bag().set("groups",
        PropertyValue::from_u8(n));
}

static void test_default_groups_property_is_1(void) {
    auto& bag = dynamic_show_property_bag();
    TEST_ASSERT_EQUAL_UINT8(1, bag.get("groups").as_u8());
}

static void test_groups_1_broadcasts_kick_to_group_0(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    set_test_millis(100);
    d->on_beat_detected(ctx, 200);

    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.count());
    TEST_ASSERT_EQUAL_UINT8(0, g_espnow_driver.last_group());
}

static void test_groups_3_kick_routes_to_group_1(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    set_groups(3);
    d->enter(ctx);

    set_test_millis(100);
    d->on_beat_detected(ctx, 200);

    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.count());
    TEST_ASSERT_EQUAL_UINT8(1, g_espnow_driver.last_group());
}

static void test_groups_3_snare_routes_to_group_2(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    set_groups(3);
    d->enter(ctx);

    set_test_millis(100);
    d->on_snare_detected(ctx, 200);

    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.count());
    TEST_ASSERT_EQUAL_UINT8(2, g_espnow_driver.last_group());
}

static void test_groups_3_hihat_routes_to_group_3(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    set_groups(3);
    d->enter(ctx);

    set_test_millis(100);
    d->on_hihat_detected(ctx, 200);

    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.count());
    TEST_ASSERT_EQUAL_UINT8(3, g_espnow_driver.last_group());
}

static void test_groups_2_merges_snare_and_hihat(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    set_groups(2);
    d->enter(ctx);

    set_test_millis(100);
    d->on_beat_detected(ctx, 200);
    TEST_ASSERT_EQUAL_UINT8(1, g_espnow_driver.last_group());

    g_espnow_driver.reset();
    d->on_snare_detected(ctx, 200);
    TEST_ASSERT_EQUAL_UINT8(2, g_espnow_driver.last_group());

    g_espnow_driver.reset();
    d->on_hihat_detected(ctx, 200);
    TEST_ASSERT_EQUAL_UINT8(2, g_espnow_driver.last_group());
}

// Master-IR loopback: the dispatch_output_class_group helper fires the
// master's ir-pixmob driver alongside the ESP-NOW broadcast whenever
// target_class is 0 or 1. Tests that opt into the loopback see an
// ir-pixmob send call per render_fx.
static void test_kick_fires_master_ir_via_loopback(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // Non-zero centroid + energy so the colour math doesn't bottom out
    // at the rgb floor and skip the IR-loopback's rgb-non-zero gate.
    d->on_music_descriptor(ctx, /*c=*/128, /*e=*/200, /*d=*/100);
    set_test_millis(100);
    d->on_beat_detected(ctx, 200);

    // ESP-NOW broadcast fires once (groups=1 default -> 00:00).
    TEST_ASSERT_EQUAL_INT(1, g_espnow_driver.count());
    // Master IR fires once via the dispatch loopback (target_class=0
    // -> wildcard, ir-pixmob driver enabled).
    TEST_ASSERT_EQUAL_INT(1, g_ir_driver.count());
}

// =============================================================================
// Colour math: centroid shifts hue from blue (low) to red (high).
// =============================================================================

static void test_low_centroid_fires_cool_colour(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // Low centroid (bass-heavy), full energy.
    d->on_music_descriptor(ctx, /*c=*/0, /*e=*/255, /*d=*/100);
    d->on_beat_detected(ctx, 200);

    const auto ev = g_espnow_driver.last_event();
    // Hue 240 -> blue. blue > red.
    TEST_ASSERT_GREATER_THAN_UINT8(ev.r, ev.b);
}

static void test_high_centroid_fires_warm_colour(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // High centroid (treble-heavy), full energy.
    d->on_music_descriptor(ctx, /*c=*/255, /*e=*/255, /*d=*/100);
    d->on_beat_detected(ctx, 200);

    const auto ev = g_espnow_driver.last_event();
    // Hue 0 -> red. red > blue.
    TEST_ASSERT_GREATER_THAN_UINT8(ev.b, ev.r);
}

// =============================================================================
// Section: DROP overrides to white.
// =============================================================================

static void test_drop_section_fires_white(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // Even with a low centroid, DROP forces white.
    d->on_music_descriptor(ctx, /*c=*/0, /*e=*/0, /*d=*/0);
    d->on_section_change(ctx,
        static_cast<uint8_t>(dal::analyser::SectionType::Drop));
    d->on_beat_detected(ctx, 200);

    const auto ev = g_espnow_driver.last_event();
    TEST_ASSERT_EQUAL_UINT8(255, ev.r);
    TEST_ASSERT_EQUAL_UINT8(255, ev.g);
    TEST_ASSERT_EQUAL_UINT8(255, ev.b);
}

static void test_breakdown_section_dims_output(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // CHORUS at full energy fires a bright colour.
    d->on_music_descriptor(ctx, /*c=*/200, /*e=*/255, /*d=*/100);
    d->on_section_change(ctx,
        static_cast<uint8_t>(dal::analyser::SectionType::Chorus));
    set_test_millis(100);
    d->on_beat_detected(ctx, 200);
    const auto chorus_ev = g_espnow_driver.last_event();
    const int chorus_total = static_cast<int>(chorus_ev.r) +
                              static_cast<int>(chorus_ev.g) +
                              static_cast<int>(chorus_ev.b);

    // BREAKDOWN should dim. Reset driver counters to isolate the new fire.
    g_espnow_driver.reset();
    d->on_section_change(ctx,
        static_cast<uint8_t>(dal::analyser::SectionType::Breakdown));
    set_test_millis(800);   // outside any refractory-style implicit gating
    d->on_beat_detected(ctx, 200);
    const auto bd_ev = g_espnow_driver.last_event();
    const int bd_total = static_cast<int>(bd_ev.r) +
                          static_cast<int>(bd_ev.g) +
                          static_cast<int>(bd_ev.b);

    TEST_ASSERT_LESS_THAN_INT(chorus_total, bd_total);
}

// =============================================================================
// Density: low density -> low chance, high density -> high chance.
// =============================================================================

static void test_low_density_picks_low_chance(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    d->on_music_descriptor(ctx, /*c=*/128, /*e=*/128, /*d=*/0);
    d->on_beat_detected(ctx, 200);
    const uint8_t low_chance = static_cast<uint8_t>(
        g_espnow_driver.last_event().chance);

    g_espnow_driver.reset();
    set_test_millis(500);
    d->on_music_descriptor(ctx, /*c=*/128, /*e=*/128, /*d=*/255);
    d->on_beat_detected(ctx, 200);
    const uint8_t high_chance = static_cast<uint8_t>(
        g_espnow_driver.last_event().chance);

    // CHANCE enum values: 0=100%, 1=88%, 2=67%, 3=50%, 4=32%, 5=16%,
    // 6=10%, 7=4%. So "higher density -> lower numeric chance value".
    TEST_ASSERT_LESS_THAN_UINT8(low_chance, high_chance);
}

// =============================================================================
// Paused suppresses fires.
// =============================================================================

static void test_paused_suppresses_render(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    ctx.set_paused(true);
    set_test_millis(100);
    d->on_beat_detected(ctx, 200);
    d->on_snare_detected(ctx, 200);
    d->on_hihat_detected(ctx, 200);

    TEST_ASSERT_EQUAL_INT(0, g_espnow_driver.count());
}

// =============================================================================
// BPM tracking: 120 BPM kick train -> estimated_bpm ~ 120.
// =============================================================================

static void test_bpm_tracking_from_kick_ibi(void) {
    DynamicShow* d = dynamic_show_instance();
    auto& ctx = dynamic_show_context();
    d->enter(ctx);

    // 4 kicks at 500 ms spacing = 120 BPM.
    for (int i = 1; i <= 4; ++i) {
        set_test_millis(i * 500);
        d->on_beat_detected(ctx, 200);
    }
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, d->estimated_bpm_for_tests());
}

// =============================================================================
// Unity main
// =============================================================================

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_identity);
    RUN_TEST(test_required_capabilities_includes_mic);
    RUN_TEST(test_default_groups_property_is_1);
    RUN_TEST(test_groups_1_broadcasts_kick_to_group_0);
    RUN_TEST(test_groups_3_kick_routes_to_group_1);
    RUN_TEST(test_groups_3_snare_routes_to_group_2);
    RUN_TEST(test_groups_3_hihat_routes_to_group_3);
    RUN_TEST(test_groups_2_merges_snare_and_hihat);
    RUN_TEST(test_kick_fires_master_ir_via_loopback);
    RUN_TEST(test_low_centroid_fires_cool_colour);
    RUN_TEST(test_high_centroid_fires_warm_colour);
    RUN_TEST(test_drop_section_fires_white);
    RUN_TEST(test_breakdown_section_dims_output);
    RUN_TEST(test_low_density_picks_low_chance);
    RUN_TEST(test_paused_suppresses_render);
    RUN_TEST(test_bpm_tracking_from_kick_ibi);
    return UNITY_END();
}
