// Native test: widget library (Epic 4.7 Block 2).
//
// BeatBarWidget + SpectrumBarsWidget are pure render helpers - they
// take a value, optionally a marker / sensitivity, and emit
// DAL::fire_display_* calls at the requested coordinates. The tests
// here cover the value-clamping contract, the roll-up math, and the
// state-after-update accessor surface. Pixel-level rendering is
// verified by hardware bring-up; the math + clamping is what matters
// for correctness.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "dal/dal.h"
#include "widgets/beat_bar.h"
#include "widgets/spectrum_bars.h"

// =============================================================================
// Test HAL backend. Minimal Display + no other capabilities. The
// widgets' draw() emits fire_display_*; we don't verify the resulting
// framebuffer here, just that the calls compile and the math is right.
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

static constexpr Capability kCapabilities[] = { Capability::Display };
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

}  // namespace hal
}  // namespace nocturnation

using namespace nocturnation;
using nocturnation::widgets::BeatBarWidget;
using nocturnation::widgets::SpectrumBarsWidget;
using nocturnation::widgets::kSpectrumBandCount;

// =============================================================================
// Unity setup / teardown
// =============================================================================

void setUp(void) {
    dal::DAL::begin();
}

void tearDown(void) {}

// =============================================================================
// BeatBarWidget
// =============================================================================

static void test_beat_bar_defaults_to_zero(void) {
    BeatBarWidget w;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.bar_fraction());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.marker_fraction());
}

static void test_beat_bar_update_within_range(void) {
    BeatBarWidget w;
    w.update(0.5f, 0.7f);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, w.bar_fraction());
    TEST_ASSERT_EQUAL_FLOAT(0.7f, w.marker_fraction());
}

static void test_beat_bar_update_clamps_above_one(void) {
    BeatBarWidget w;
    w.update(2.5f, 3.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, w.bar_fraction());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, w.marker_fraction());
}

static void test_beat_bar_update_clamps_below_zero(void) {
    BeatBarWidget w;
    w.update(-0.5f, -1.0f);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.bar_fraction());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, w.marker_fraction());
}

static void test_beat_bar_draw_at_zero_does_not_crash(void) {
    // Edge: zero-area rectangle. Widget should early-return.
    BeatBarWidget w;
    w.update(0.5f);
    w.draw(10, 10, 0, 0);
    w.draw(10, 10, 3, 3);   // below the minimum 4x4
    // No assertion - we're verifying it doesn't trap on the path.
    TEST_ASSERT_TRUE(true);
}

static void test_beat_bar_draw_emits_calls(void) {
    // Issue a draw at full canvas with the default LocalDriver claiming
    // "local". driver_send_count("local") increments on each
    // fire_display_fill_rect, so we can verify the widget produced
    // some output. Exact pixel positions aren't asserted - hardware
    // bring-up verifies visual correctness.
    const uint32_t before = dal::DAL::driver_send_count("local");
    BeatBarWidget w;
    w.update(0.5f, 0.25f);
    w.draw(0, 0, 240, 14);
    const uint32_t after = dal::DAL::driver_send_count("local");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(5u, after - before);
    // Expected: 4 frame edges + 1 fill bar + 1 marker = 6 fill_rect
    // calls when both bar and marker are non-zero. Lower bound 5
    // accommodates edge cases (e.g., marker at the same x as the
    // frame edge).
}

// =============================================================================
// SpectrumBarsWidget
// =============================================================================

static void test_spectrum_defaults_to_zero(void) {
    SpectrumBarsWidget w;
    for (size_t i = 0; i < kSpectrumBandCount; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, w.band_value(i));
    }
}

static void test_spectrum_update_clamps_to_unit_interval(void) {
    SpectrumBarsWidget w;
    const float values[kSpectrumBandCount] = {
        -1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 2.0f
    };
    w.update(values);
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  w.band_value(0));   // clamped from -1
    TEST_ASSERT_EQUAL_FLOAT(0.0f,  w.band_value(1));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, w.band_value(2));
    TEST_ASSERT_EQUAL_FLOAT(0.5f,  w.band_value(3));
    TEST_ASSERT_EQUAL_FLOAT(0.75f, w.band_value(4));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  w.band_value(5));
    TEST_ASSERT_EQUAL_FLOAT(1.0f,  w.band_value(6));   // clamped from 2
}

static void test_spectrum_roll_up_silence_returns_zeros(void) {
    float mags[32] = {0};
    float out[kSpectrumBandCount];
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 5, out);
    for (size_t i = 0; i < kSpectrumBandCount; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, out[i]);
    }
}

static void test_spectrum_roll_up_loud_kick_band_only(void) {
    // Loud sub-bass / bass content; mids and highs silent. Per the
    // kBandMap, bands 0..3 -> Sub Bass, 4..11 -> Bass. Set those to
    // 1M magnitude each (loud-music range) and verify Sub Bass +
    // Bass perceptual values are non-zero while higher bands are 0.
    float mags[32] = {0};
    for (size_t i = 0; i < 12; ++i) mags[i] = 1000000.0f;
    float out[kSpectrumBandCount];
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 5, out);
    TEST_ASSERT_TRUE(out[0] > 0.0f);   // Sub Bass
    TEST_ASSERT_TRUE(out[1] > 0.0f);   // Bass
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[2]);   // Low Mids
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[3]);   // Midrange
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[4]);   // High Mids
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[5]);   // Presence
    TEST_ASSERT_EQUAL_FLOAT(0.0f, out[6]);   // Air
}

static void test_spectrum_roll_up_sensitivity_zero_clamps_to_one(void) {
    // sensitivity 0 -> 1 internally. Two calls (sens=0 vs sens=1)
    // should produce the same output for the same input.
    float mags[32] = {0};
    for (size_t i = 0; i < 12; ++i) mags[i] = 1000000.0f;
    float out0[kSpectrumBandCount];
    float out1[kSpectrumBandCount];
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 0, out0);
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 1, out1);
    for (size_t i = 0; i < kSpectrumBandCount; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(out1[i], out0[i]);
    }
}

static void test_spectrum_roll_up_higher_sensitivity_higher_values(void) {
    // Same input at sens=1 vs sens=10. Higher sensitivity should
    // produce larger output values (until clamped). At least one
    // band must rise.
    float mags[32] = {0};
    for (size_t i = 0; i < 12; ++i) mags[i] = 100000.0f;
    float out_low[kSpectrumBandCount];
    float out_high[kSpectrumBandCount];
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 1, out_low);
    SpectrumBarsWidget::roll_up_spectrum_to_perceptual(mags, 10, out_high);
    bool any_higher = false;
    for (size_t i = 0; i < kSpectrumBandCount; ++i) {
        if (out_high[i] > out_low[i]) any_higher = true;
    }
    TEST_ASSERT_TRUE(any_higher);
}

static void test_spectrum_draw_emits_calls(void) {
    const uint32_t before = dal::DAL::driver_send_count("local");
    SpectrumBarsWidget w;
    float values[kSpectrumBandCount] = {0.3f, 0.5f, 0.7f, 0.4f, 0.6f, 0.8f, 0.2f};
    w.update(values);
    w.draw(0, 14, 240, 110);
    const uint32_t after = dal::DAL::driver_send_count("local");
    // Expected: 1 background clear + 7*2 bar segments (floor + main) +
    // 7 label texts = at least 15 calls. Lower bound 8 to be tolerant
    // of small geometry tweaks.
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(8u, after - before);
}

static void test_spectrum_draw_at_tiny_size_does_not_crash(void) {
    SpectrumBarsWidget w;
    float values[kSpectrumBandCount] = {0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f};
    w.update(values);
    w.draw(0, 0, 16, 8);   // below minimum, should early-return
    TEST_ASSERT_TRUE(true);
}

// =============================================================================
// Unity main
// =============================================================================

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_beat_bar_defaults_to_zero);
    RUN_TEST(test_beat_bar_update_within_range);
    RUN_TEST(test_beat_bar_update_clamps_above_one);
    RUN_TEST(test_beat_bar_update_clamps_below_zero);
    RUN_TEST(test_beat_bar_draw_at_zero_does_not_crash);
    RUN_TEST(test_beat_bar_draw_emits_calls);
    RUN_TEST(test_spectrum_defaults_to_zero);
    RUN_TEST(test_spectrum_update_clamps_to_unit_interval);
    RUN_TEST(test_spectrum_roll_up_silence_returns_zeros);
    RUN_TEST(test_spectrum_roll_up_loud_kick_band_only);
    RUN_TEST(test_spectrum_roll_up_sensitivity_zero_clamps_to_one);
    RUN_TEST(test_spectrum_roll_up_higher_sensitivity_higher_values);
    RUN_TEST(test_spectrum_draw_emits_calls);
    RUN_TEST(test_spectrum_draw_at_tiny_size_does_not_crash);
    return UNITY_END();
}
