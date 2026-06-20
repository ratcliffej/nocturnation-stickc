// Native test for LedStripDriver (Epic 12 B2).
//
// Covers:
//   - begin() initialises the strip, clears every pixel, calls show() once.
//   - send_wash with cycle_ms == 0 holds (r1,g1,b1) on every pixel after
//     the attack ramp settles.
//   - send_wash attack ramp: at attack-midpoint the rendered RGB is ~50 %
//     of the final wash colour.
//   - send_wash with cycle_ms > 0 cosine-drifts: at quarter-cycle the
//     mix is ~0.5 between r1 and r2.
//   - send_wash_end fades to (0,0,0) over release window and deactivates.
//   - send (RgbPulseEvent) spawns a sparkle on exactly one pixel; the
//     other pixels stay at the wash baseline.
//   - Sparkle fade-back: at sparkle-end the pixel returns to baseline.
//   - intensity scalar applied to baseline.
//   - Inactive driver: loop_tick with no wash and no sparkles is a no-op
//     (no pixel writes captured).
//
// Recording mock for hal::LedStrip captures every set_pixel + show()
// invocation so tests assert on actual pixel output. Clock + RNG seams
// on the driver are installed so the cosine + sparkle pixel are
// deterministic.

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unity.h>

#include "hal/hal.h"
#include "pixmob_protocol.h"
#include "dal/drivers/led_strip_driver.h"

using nocturnation::dal::LedStripDriver;
using nocturnation::dal::led_strip_driver_instance;
using nocturnation::dal::LightWashEvent;
using nocturnation::dal::RgbPulseEvent;

// =============================================================================
// HAL stub - no led_strip exposed; the driver uses the test override.
// =============================================================================

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = {
    Capability::LedStrip,
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

Mic*      HAL::mic()       { return nullptr; }
IRTx*     HAL::ir_tx()     { return nullptr; }
IRTx*     HAL::ir_tx_ext() { return nullptr; }
IRRx*     HAL::ir_rx()     { return nullptr; }
ESPNow*   HAL::esp_now()   { return nullptr; }
Display*  HAL::display()   { return nullptr; }
Buttons*  HAL::buttons()   { return nullptr; }
IMU*      HAL::imu()       { return nullptr; }
Battery*  HAL::battery()   { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Recording mock LedStrip
// =============================================================================

namespace {

constexpr size_t kPixelCount = 8;

struct RecordingStrip : public nocturnation::hal::LedStrip {
    struct Pixel { uint8_t r = 0, g = 0, b = 0; };

    bool   begun       = false;
    int    begin_count = 0;
    int    show_count  = 0;
    int    clear_count = 0;
    int    set_pixel_count = 0;
    Pixel  pixels[kPixelCount];

    void begin() override {
        begun = true;
        ++begin_count;
    }
    size_t pixel_count() const override { return kPixelCount; }
    void set_pixel(size_t i, uint8_t r, uint8_t g, uint8_t b) override {
        if (i >= kPixelCount) return;
        pixels[i].r = r;
        pixels[i].g = g;
        pixels[i].b = b;
        ++set_pixel_count;
    }
    void clear() override {
        for (auto& p : pixels) p = Pixel{};
        ++clear_count;
    }
    void show() override { ++show_count; }

    void reset_counters() {
        begin_count = show_count = clear_count = set_pixel_count = 0;
    }
};

RecordingStrip s_strip;

uint32_t s_now_ms = 0;
uint32_t test_now() { return s_now_ms; }

// Deterministic RNG that walks through a small known sequence so tests
// can predict which pixel a sparkle lands on.
uint32_t s_rng_seq[8] = { 3, 5, 0, 7, 1, 6, 4, 2 };
size_t   s_rng_idx    = 0;
uint32_t test_rng() {
    const uint32_t v = s_rng_seq[s_rng_idx % 8];
    ++s_rng_idx;
    return v;
}

void install_seams(LedStripDriver* drv) {
    drv->set_strip_override(&s_strip);
    drv->set_clock_source(test_now);
    drv->set_rng_source(test_rng);
}

LightWashEvent make_wash(uint8_t r1, uint8_t g1, uint8_t b1,
                          uint8_t r2 = 0, uint8_t g2 = 0, uint8_t b2 = 0,
                          uint16_t cycle_ms = 0,
                          uint8_t intensity = 255,
                          uint8_t attack = 0,
                          uint8_t release = 0) {
    LightWashEvent ev{};
    ev.r1 = r1; ev.g1 = g1; ev.b1 = b1;
    ev.r2 = r2; ev.g2 = g2; ev.b2 = b2;
    ev.cycle_ms      = cycle_ms;
    ev.intensity     = intensity;
    ev.attack        = attack;
    ev.release       = release;
    ev.ttl_seconds   = 0;
    ev.pulse_response = 1;
    return ev;
}

void reset_driver() {
    auto* drv = led_strip_driver_instance();
    // Cancel any leftover wash from a previous test.
    drv->send_wash_end(0, 0);
    // Tick to flush release transition.
    s_now_ms += 10;
    drv->loop_tick();
    // Reset mock + RNG indices.
    s_strip = RecordingStrip{};
    s_rng_idx = 0;
}

}  // namespace

// =============================================================================
// Fixtures
// =============================================================================

void setUp(void) {
    s_now_ms = 1000;
    s_rng_idx = 0;
    s_strip = RecordingStrip{};
    auto* drv = led_strip_driver_instance();
    install_seams(drv);
    TEST_ASSERT_TRUE(drv->begin());
    // Existing wash/sparkle tests assert on pixel 0 = baseline; the
    // signal indicator overlay (Epic 12 B5) would interfere because
    // the test HAL has no Display capability and begin() auto-enables
    // the indicator. Disable here; the indicator suite below re-
    // enables explicitly per test.
    drv->set_signal_indicator_enabled(false);
    drv->reset_indicator_for_tests();
    reset_driver();
    s_now_ms = 1000;
    install_seams(led_strip_driver_instance());
    led_strip_driver_instance()->set_signal_indicator_enabled(false);
    led_strip_driver_instance()->reset_indicator_for_tests();
}

void tearDown(void) {}

// =============================================================================
// Tests
// =============================================================================

static void test_begin_initialises_strip(void) {
    // setUp ran begin() which calls strip.begin + clear + show. After
    // reset_driver() the counters were reset; we re-begin here to see
    // a fresh sequence.
    auto* drv = led_strip_driver_instance();
    s_strip.reset_counters();
    TEST_ASSERT_TRUE(drv->begin());
    TEST_ASSERT_EQUAL_INT(1, s_strip.begin_count);
    TEST_ASSERT_EQUAL_INT(1, s_strip.clear_count);
    TEST_ASSERT_TRUE(s_strip.show_count >= 1);
}

static void test_wash_hold_paints_every_pixel(void) {
    auto* drv = led_strip_driver_instance();
    // Hold red, no cycle, no attack -> single tick paints all pixels red.
    drv->send_wash(0, make_wash(200, 0, 0));
    s_strip.reset_counters();
    drv->loop_tick();

    TEST_ASSERT_EQUAL_INT((int)kPixelCount, s_strip.set_pixel_count);
    for (size_t i = 0; i < kPixelCount; ++i) {
        TEST_ASSERT_EQUAL_UINT8(200, s_strip.pixels[i].r);
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].b);
    }
    TEST_ASSERT_TRUE(s_strip.show_count >= 1);
}

static void test_wash_attack_ramp_mid_is_half_intensity(void) {
    auto* drv = led_strip_driver_instance();
    // attack = 10 (1000 ms), hold red 200. At t = started + 500 ms the
    // attack is at 50%, so the rendered red should be ~100. (Permissive
    // bounds because the lerp + intensity-scale rounding can shift by 1.)
    drv->send_wash(0, make_wash(200, 0, 0, 0, 0, 0,
                                  /*cycle_ms=*/0, /*intensity=*/255,
                                  /*attack=*/10, /*release=*/0));
    s_now_ms += 500;
    s_strip.reset_counters();
    drv->loop_tick();

    const uint8_t r0 = s_strip.pixels[0].r;
    TEST_ASSERT_TRUE_MESSAGE(r0 > 80 && r0 < 120,
        "attack mid-ramp red should be ~100 (200 * 0.5)");
}

static void test_wash_drift_quarter_cycle_blends(void) {
    auto* drv = led_strip_driver_instance();
    // cycle_ms = 4000, r1 = (200,0,0), r2 = (0,0,200). Quarter cycle is
    // t=1000. cos(pi/2) = 0; mix = 0.5. Expected RGB = (100, 0, 100).
    drv->send_wash(0, make_wash(200, 0, 0, 0, 0, 200,
                                  /*cycle_ms=*/4000, /*intensity=*/255));
    s_now_ms += 1000;
    s_strip.reset_counters();
    drv->loop_tick();

    const uint8_t r0 = s_strip.pixels[0].r;
    const uint8_t b0 = s_strip.pixels[0].b;
    TEST_ASSERT_TRUE_MESSAGE(r0 > 90 && r0 < 110,
        "drift quarter-cycle r should be ~100");
    TEST_ASSERT_TRUE_MESSAGE(b0 > 90 && b0 < 110,
        "drift quarter-cycle b should be ~100");
}

static void test_wash_end_fades_to_black(void) {
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(200, 0, 0));
    s_now_ms += 100;
    drv->loop_tick();   // paint baseline
    TEST_ASSERT_EQUAL_UINT8(200, s_strip.pixels[0].r);

    // End with release = 10 (1000 ms). At end-of-release the pixel
    // should be at or near zero.
    drv->send_wash_end(0, /*release_time=*/10);
    s_now_ms += 1001;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[0].r);
    TEST_ASSERT_NULL(drv->wash_state());     // deactivated
}

static void test_sparkle_lands_on_one_pixel(void) {
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(0, 100, 0));   // green baseline
    s_now_ms += 50;
    drv->loop_tick();
    // First sparkle: RNG returns 3 -> pixel 3.
    RgbPulseEvent sp{};
    sp.r = 255; sp.g = 255; sp.b = 255;
    sp.attack = pixmob::T_32_MS;
    sp.sustain = pixmob::T_192_MS;
    sp.release = pixmob::T_32_MS;   // total ~256 ms
    sp.chance = pixmob::CHANCE_100;   // always fire
    drv->send(0, sp);

    // Render immediately after spawn - sparkle should be at ~peak.
    s_strip.reset_counters();
    drv->loop_tick();
    int bright_count = 0, baseline_count = 0;
    size_t bright_idx = kPixelCount;
    for (size_t i = 0; i < kPixelCount; ++i) {
        const auto& p = s_strip.pixels[i];
        if (p.r > 100 && p.g > 100 && p.b > 100) {
            ++bright_count;
            bright_idx = i;
        } else if (p.r == 0 && p.g == 100 && p.b == 0) {
            ++baseline_count;
        }
    }
    TEST_ASSERT_EQUAL_INT(1, bright_count);
    TEST_ASSERT_EQUAL_INT((int)(kPixelCount - 1), baseline_count);
    // First test_rng() consume = 3. setUp's reset_driver consumed entries
    // earlier; we reset s_rng_idx in setUp so the next consume returns
    // s_rng_seq[0] = 3. Confirms the sparkle lands on pixel 3.
    TEST_ASSERT_EQUAL_INT(3, (int)bright_idx);
}

static void test_sparkle_fade_returns_to_baseline(void) {
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(0, 100, 0));
    s_now_ms += 50;
    drv->loop_tick();

    RgbPulseEvent sp{};
    sp.r = 255; sp.g = 255; sp.b = 255;
    sp.attack = pixmob::T_0_MS;
    sp.sustain = pixmob::T_192_MS;
    sp.release = pixmob::T_0_MS;   // ~192 ms total
    sp.chance = pixmob::CHANCE_100;
    drv->send(0, sp);

    // Advance past sparkle duration; render. Pixel 3 should be back to baseline.
    s_now_ms += 500;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[3].r);
    TEST_ASSERT_EQUAL_UINT8(100, s_strip.pixels[3].g);
    TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[3].b);
}

static void test_intensity_scalar_applies(void) {
    auto* drv = led_strip_driver_instance();
    // Red 200 at half intensity (127) -> expect ~99.
    drv->send_wash(0, make_wash(200, 0, 0, 0, 0, 0,
                                  /*cycle_ms=*/0, /*intensity=*/127));
    s_now_ms += 50;
    s_strip.reset_counters();
    drv->loop_tick();
    const uint8_t r0 = s_strip.pixels[0].r;
    TEST_ASSERT_TRUE_MESSAGE(r0 > 90 && r0 < 110,
        "intensity 127 / 255 * 200 = ~99");
}

// =============================================================================
// Signal indicator (Epic 12 B5)
// =============================================================================

static void test_indicator_auto_enabled_when_no_display(void) {
    // setUp() turns the indicator off explicitly; begin() auto-enables
    // it based on HAL::Display absence. Re-begin and confirm.
    auto* drv = led_strip_driver_instance();
    TEST_ASSERT_TRUE(drv->begin());
    TEST_ASSERT_TRUE(drv->signal_indicator_enabled());
    drv->set_signal_indicator_enabled(false);   // restore test default
}

static void test_indicator_searching_pulses_green_on_pixel_0(void) {
    auto* drv = led_strip_driver_instance();
    drv->set_signal_indicator_enabled(true);

    // Phase 0 (within first half of the 1000 ms period) -> lit.
    s_now_ms = 1000;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].r);
    TEST_ASSERT_TRUE_MESSAGE(s_strip.pixels[0].g > 0,
        "searching first-half should light pixel 0 green");
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].b);

    // Phase ~600 ms -> dark half.
    s_now_ms = 1600;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[0].g);
    TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[0].b);

    // Other pixels stay black throughout (no wash, no sparkles).
    for (size_t i = 1; i < kPixelCount; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[i].r);
        TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[i].b);
    }
    drv->set_signal_indicator_enabled(false);
}

static void test_indicator_fresh_lock_then_yields_to_wash(void) {
    auto* drv = led_strip_driver_instance();
    drv->set_signal_indicator_enabled(true);

    // First wash arrives -> FreshlyLocked. Solid green on pixel 0 for
    // kFreshLockDurationMs (1000 ms) regardless of wash colour.
    drv->send_wash(0, make_wash(255, 0, 0));      // red baseline
    s_now_ms += 1;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT((int)LedStripDriver::IndicatorState::FreshlyLocked,
                          (int)drv->indicator_state_for_tests());
    TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[0].r);
    TEST_ASSERT_TRUE(s_strip.pixels[0].g > 0);
    TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[0].b);
    // Other pixels show wash baseline (red).
    TEST_ASSERT_EQUAL_UINT8(255, s_strip.pixels[1].r);

    // After the fresh-lock window: state moves to Active and pixel 0
    // belongs to the wash render.
    s_now_ms += 1100;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT((int)LedStripDriver::IndicatorState::Active,
                          (int)drv->indicator_state_for_tests());
    TEST_ASSERT_EQUAL_UINT8(255, s_strip.pixels[0].r);

    drv->set_signal_indicator_enabled(false);
}

static void test_indicator_lost_signal_returns_to_searching(void) {
    auto* drv = led_strip_driver_instance();
    drv->set_signal_indicator_enabled(true);

    // Bring up a lock, then go past the no-signal threshold.
    drv->send_wash(0, make_wash(0, 0, 255));
    s_now_ms += 1100;
    drv->loop_tick();   // promote to Active
    TEST_ASSERT_EQUAL_INT((int)LedStripDriver::IndicatorState::Active,
                          (int)drv->indicator_state_for_tests());

    // Advance well past kNoSignalThresholdMs without new washes.
    s_now_ms += 4000;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT((int)LedStripDriver::IndicatorState::Searching,
                          (int)drv->indicator_state_for_tests());

    drv->set_signal_indicator_enabled(false);
}

// =============================================================================
// Runner
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_begin_initialises_strip);
    RUN_TEST(test_wash_hold_paints_every_pixel);
    RUN_TEST(test_wash_attack_ramp_mid_is_half_intensity);
    RUN_TEST(test_wash_drift_quarter_cycle_blends);
    RUN_TEST(test_wash_end_fades_to_black);
    RUN_TEST(test_sparkle_lands_on_one_pixel);
    RUN_TEST(test_sparkle_fade_returns_to_baseline);
    RUN_TEST(test_intensity_scalar_applies);
    RUN_TEST(test_indicator_auto_enabled_when_no_display);
    RUN_TEST(test_indicator_searching_pulses_green_on_pixel_0);
    RUN_TEST(test_indicator_fresh_lock_then_yields_to_wash);
    RUN_TEST(test_indicator_lost_signal_returns_to_searching);
    return UNITY_END();
}
