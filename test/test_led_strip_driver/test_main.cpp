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
uint8_t HAL::max_strip_brightness_percent() { return 100; }

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
    // Clear the pixel-0 overlay between tests so a previous test's
    // override doesn't leak into the next one's baseline assertions.
    drv->set_overlay_pixel_0(0, 0, 0, false);
    // Default brightness to 100 % so existing wash/sparkle tests
    // (written before brightness landed) keep their literal RGB
    // assertions. The brightness-specific tests below override this.
    drv->set_brightness_percent(100);
    reset_driver();
    s_now_ms = 1000;
    install_seams(led_strip_driver_instance());
    led_strip_driver_instance()->set_overlay_pixel_0(0, 0, 0, false);
    led_strip_driver_instance()->set_brightness_percent(100);
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

static void test_chance_100_flashes_every_pixel(void) {
    // CHANCE_100 should hit every pixel - effectively a whole-strip
    // flash. Treats each pixel as a bracelet that always rolls a hit.
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(0, 100, 0));   // green baseline
    s_now_ms += 50;
    drv->loop_tick();

    RgbPulseEvent sp{};
    sp.r = 255; sp.g = 255; sp.b = 255;
    sp.attack = pixmob::T_0_MS;
    sp.sustain = pixmob::T_192_MS;
    sp.release = pixmob::T_0_MS;
    sp.chance = pixmob::CHANCE_100;
    drv->send(0, sp);

    drv->loop_tick();
    // Mid-sustain (we're at start of the 192ms sustain) - every pixel
    // should be at peak (255,255,255).
    for (size_t i = 0; i < kPixelCount; ++i) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(255, s_strip.pixels[i].r,
            "every pixel should peak at pulse colour with CHANCE_100");
        TEST_ASSERT_EQUAL_UINT8(255, s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(255, s_strip.pixels[i].b);
    }
}

static void test_chance_0_filters_all_pixels(void) {
    // A non-CHANCE_100 chance uses the RNG; a chance of 0 percent never
    // hits, so no pixel lights up. (CHANCE_4 is the protocol's lowest
    // bucket; a properly-mocked 0-roll-always-fails sequence demonstrates
    // the same gating logic.)
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(0, 100, 0));
    s_now_ms += 50;
    drv->loop_tick();

    // Override RNG to return 99 always (above any chance percent except 100).
    static auto always_99 = []() -> uint32_t { return 99; };
    drv->set_rng_source(always_99);

    RgbPulseEvent sp{};
    sp.r = 255; sp.g = 255; sp.b = 255;
    sp.attack = pixmob::T_0_MS;
    sp.sustain = pixmob::T_192_MS;
    sp.release = pixmob::T_0_MS;
    sp.chance = pixmob::CHANCE_88;   // 88 percent - rolls of 99 always fail
    drv->send(0, sp);

    drv->loop_tick();
    // No pixel should be lit beyond baseline.
    for (size_t i = 0; i < kPixelCount; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].r);
        TEST_ASSERT_EQUAL_UINT8(100, s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].b);
    }

    // Restore deterministic RNG for subsequent tests.
    drv->set_rng_source(test_rng);
}

static void test_pulse_fade_returns_pixels_to_baseline(void) {
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(0, 100, 0));
    s_now_ms += 50;
    drv->loop_tick();

    RgbPulseEvent sp{};
    sp.r = 255; sp.g = 255; sp.b = 255;
    sp.attack = pixmob::T_0_MS;
    sp.sustain = pixmob::T_192_MS;
    sp.release = pixmob::T_0_MS;
    sp.chance = pixmob::CHANCE_100;
    drv->send(0, sp);

    // Advance past envelope. Every pixel back to wash baseline.
    s_now_ms += 500;
    drv->loop_tick();
    for (size_t i = 0; i < kPixelCount; ++i) {
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].r);
        TEST_ASSERT_EQUAL_UINT8(100, s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[i].b);
    }
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
// Pixel 0 overlay API (Epic 12 B5, DRY refactor)
// =============================================================================
//
// The signal-state policy moved into LumeMode; the driver is now a
// pure mechanism layer. Confirm the overlay API itself works:
// enabled overlay wins over the wash baseline, disabled overlay
// yields pixel 0 back to the wash render.

static void test_overlay_writes_pixel_0_over_wash(void) {
    auto* drv = led_strip_driver_instance();
    // Hold a red wash.
    drv->send_wash(0, make_wash(200, 0, 0));
    s_now_ms += 50;

    // Enable a green overlay on pixel 0.
    drv->set_overlay_pixel_0(0, 96, 0, true);
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(96, s_strip.pixels[0].g);
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].b);
    // Other pixels still show the wash.
    TEST_ASSERT_EQUAL_UINT8(200, s_strip.pixels[1].r);
}

static void test_brightness_scales_wash(void) {
    // Wash red 200 at device brightness 10 % -> renders ~20 on every pixel.
    auto* drv = led_strip_driver_instance();
    drv->set_brightness_percent(10);
    drv->send_wash(0, make_wash(200, 0, 0));
    s_now_ms += 50;
    drv->loop_tick();
    for (size_t i = 0; i < kPixelCount; ++i) {
        const uint8_t r = s_strip.pixels[i].r;
        TEST_ASSERT_TRUE_MESSAGE(r >= 18 && r <= 22,
            "wash at 10 % brightness should land near 20/255 red");
        TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[i].g);
        TEST_ASSERT_EQUAL_UINT8(0, s_strip.pixels[i].b);
    }
}

static void test_brightness_does_not_scale_overlay(void) {
    // The pixel-0 overlay is system UI and must remain at the colour
    // LumeMode passes in regardless of device brightness. Use 1 %
    // (the lowest cycle level) as the extreme case - the overlay
    // should still be visible at literally 1 % of-everything-else.
    auto* drv = led_strip_driver_instance();
    drv->set_brightness_percent(1);
    drv->send_wash(0, make_wash(200, 0, 0));
    drv->set_overlay_pixel_0(0, 96, 0, true);
    s_now_ms += 50;
    drv->loop_tick();
    // Pixel 0 = the overlay's literal 96 g (not 96 * 1 % = 1).
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(96, s_strip.pixels[0].g);
    TEST_ASSERT_EQUAL_UINT8(0,  s_strip.pixels[0].b);
    // Other pixels are wash at 1 % - red 200 * 0.01 = 2/255.
    const uint8_t r1 = s_strip.pixels[1].r;
    TEST_ASSERT_TRUE_MESSAGE(r1 <= 3,
        "wash at 1 % should be near-zero red, overlay should be untouched");
}

static void test_max_brightness_default_is_100(void) {
    // Fresh driver instance defaults to no cap (100 %).
    auto* drv = led_strip_driver_instance();
    drv->set_max_brightness_percent(100);   // explicit (other tests may have lowered it)
    TEST_ASSERT_EQUAL_UINT8(100, drv->max_brightness_percent());
    drv->set_brightness_percent(75);
    TEST_ASSERT_EQUAL_UINT8(75, drv->brightness_percent());
}

static void test_max_brightness_caps_set_brightness(void) {
    // The Atom Lite case: cap at 10 %, set to 50 %, observe clamp.
    auto* drv = led_strip_driver_instance();
    drv->set_max_brightness_percent(10);
    drv->set_brightness_percent(50);
    TEST_ASSERT_EQUAL_UINT8(10, drv->brightness_percent());
    // Setting BELOW the cap is unchanged.
    drv->set_brightness_percent(5);
    TEST_ASSERT_EQUAL_UINT8(5, drv->brightness_percent());
    // Restore default for following tests.
    drv->set_max_brightness_percent(100);
}

static void test_max_brightness_clamps_existing_value(void) {
    // Sequence: set brightness HIGH, then drop the max cap. The
    // already-applied brightness should immediately clamp - the
    // cap is hardware safety, not advisory; it can't wait for the
    // next set_brightness_percent call.
    auto* drv = led_strip_driver_instance();
    drv->set_max_brightness_percent(100);
    drv->set_brightness_percent(75);
    TEST_ASSERT_EQUAL_UINT8(75, drv->brightness_percent());
    drv->set_max_brightness_percent(10);
    TEST_ASSERT_EQUAL_UINT8(10, drv->brightness_percent());
    drv->set_max_brightness_percent(100);   // restore for following tests
}

static void test_overlay_disabled_yields_pixel_0_to_wash(void) {
    auto* drv = led_strip_driver_instance();
    drv->send_wash(0, make_wash(200, 0, 0));
    s_now_ms += 50;
    drv->set_overlay_pixel_0(0, 96, 0, true);
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(96, s_strip.pixels[0].g);

    // Disable and re-tick - pixel 0 should match the rest of the strip.
    drv->set_overlay_pixel_0(0, 0, 0, false);
    drv->loop_tick();
    TEST_ASSERT_EQUAL_UINT8(200, s_strip.pixels[0].r);
    TEST_ASSERT_EQUAL_UINT8(0,   s_strip.pixels[0].g);
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
    RUN_TEST(test_chance_100_flashes_every_pixel);
    RUN_TEST(test_chance_0_filters_all_pixels);
    RUN_TEST(test_pulse_fade_returns_pixels_to_baseline);
    RUN_TEST(test_intensity_scalar_applies);
    RUN_TEST(test_overlay_writes_pixel_0_over_wash);
    RUN_TEST(test_brightness_scales_wash);
    RUN_TEST(test_brightness_does_not_scale_overlay);
    RUN_TEST(test_max_brightness_caps_set_brightness);
    RUN_TEST(test_max_brightness_clamps_existing_value);
    RUN_TEST(test_max_brightness_default_is_100);
    RUN_TEST(test_overlay_disabled_yields_pixel_0_to_wash);
    return UNITY_END();
}
