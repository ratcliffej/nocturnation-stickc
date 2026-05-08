// Native tests for the Effect class hierarchy.
//
// Each test instantiates a concrete Effect aimed at a "test-target" device,
// drives it (on_beat / loop_tick / on_audio_frame) with synthesized inputs,
// and asserts the recording TestDriver received the right RgbPulseEvents.
//
// The TestDevice + TestDriver pattern is the same as test_dal_extensibility:
// a brand-new device type registered against the existing DAL public API,
// with a recording driver. Beats and timer ticks are injected directly into
// each Effect, so tests don't need a real beat detector or millis() clock.

#include <unity.h>
#include <cstring>
#include <cstdlib>
#include "hal/hal.h"
#include "dal/dal.h"
#include "effects/effects.h"

// =============================================================================
// Minimal HAL backend (Display only)
// =============================================================================

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = { Capability::Display };
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()    { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }
bool              HAL::has(Capability c) { return c == Capability::Display; }
void              HAL::begin()           {}
void              HAL::loop_tick()       {}

Mic*     HAL::mic()      { return nullptr; }
IRTx*    HAL::ir_tx()    { return nullptr; }
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return nullptr; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// TestDevice + recording TestDriver
// =============================================================================

namespace test_device {

using namespace nocturnation::dal;

constexpr CapabilityId kOutputCaps[] = { CapabilityId::RgbPulse };

const DeviceProfile kProfile = DeviceProfile{
    /* type_id                 = */ "TestDevice",
    /* version                 = */ "1.0",
    /* transport               = */ "inert-test",
    /* output_capabilities     = */ kOutputCaps,
    /* output_capability_count = */ sizeof(kOutputCaps)/sizeof(kOutputCaps[0]),
    /* input_capabilities      = */ nullptr,
    /* input_capability_count  = */ 0,
    /* supports_groups         = */ true,
    /* max_group_id            = */ 7,
};

class TestDriver : public Driver {
public:
    const char* transport_name() const override { return "inert-test"; }
    bool        begin()                override { return true; }

    bool send(uint8_t /*group*/, const RgbPulseEvent& ev) override {
        if (count_ < kCap) buf_[count_] = ev;
        count_++;
        last_ = ev;
        return true;
    }

    void   reset()              { count_ = 0; last_ = RgbPulseEvent{}; }
    int    count() const        { return count_; }
    RgbPulseEvent last() const  { return last_; }
    RgbPulseEvent at(int i) const {
        return (i >= 0 && i < count_ && i < (int)kCap) ? buf_[i] : RgbPulseEvent{};
    }

private:
    static constexpr size_t kCap = 32;
    int           count_ = 0;
    RgbPulseEvent last_  = {};
    RgbPulseEvent buf_[kCap] = {};
};

TestDriver driver;

}  // namespace test_device

// =============================================================================
// Tests
// =============================================================================

using namespace nocturnation;

void setUp(void) {
    test_device::driver.reset();
    dal::DAL::begin();
    dal::DAL::register_device("test-target", &test_device::kProfile, /*group=*/2);
    dal::DAL::register_driver(&test_device::driver);
}

void tearDown(void) {}

// ---------- Pulse ----------

static void test_pulse_on_beat_fires_one_event_with_set_colour(void) {
    effects::Pulse pulse("test-target");
    pulse.set_colour(0xAA, 0xBB, 0xCC);
    pulse.enter();
    pulse.on_beat(/*now=*/0, /*bpm=*/0.0f);

    TEST_ASSERT_EQUAL_INT(1, test_device::driver.count());
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_UINT8(0xAA, ev.r);
    TEST_ASSERT_EQUAL_UINT8(0xBB, ev.g);
    TEST_ASSERT_EQUAL_UINT8(0xCC, ev.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_100, (int)ev.chance);
}

static void test_pulse_envelope_picks_punchy_default_for_unknown_bpm(void) {
    effects::Pulse pulse("test-target");
    pulse.set_colour(0xFF, 0, 0);
    pulse.on_beat(0, /*bpm=*/0.0f);
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS, (int)ev.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)ev.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)ev.release);
}

static void test_pulse_envelope_picks_fast_for_high_bpm(void) {
    effects::Pulse pulse("test-target");
    pulse.set_colour(0xFF, 0, 0);
    pulse.on_beat(0, /*bpm=*/180.0f);
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_0_MS,  (int)ev.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS, (int)ev.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS, (int)ev.release);
}

static void test_pulse_envelope_picks_slow_ballad_for_low_bpm(void) {
    effects::Pulse pulse("test-target");
    pulse.set_colour(0xFF, 0, 0);
    pulse.on_beat(0, /*bpm=*/80.0f);
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS,  (int)ev.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)ev.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)ev.release);
}

static void test_pulse_skips_fire_when_colour_is_off(void) {
    effects::Pulse pulse("test-target");
    pulse.set_colour(0, 0, 0);
    pulse.on_beat(0, 120.0f);
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.count());
}

// ---------- ProbabilityPulse ----------

static void test_probability_pulse_passes_chance_to_encoder(void) {
    effects::ProbabilityPulse pp("test-target", pixmob::CHANCE_16);
    pp.set_colour(0x10, 0x20, 0x30);
    pp.on_beat(0, 120.0f);

    TEST_ASSERT_EQUAL_INT(1, test_device::driver.count());
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_16, (int)ev.chance);
    TEST_ASSERT_EQUAL_UINT8(0x10, ev.r);
    TEST_ASSERT_EQUAL_UINT8(0x20, ev.g);
    TEST_ASSERT_EQUAL_UINT8(0x30, ev.b);
}

// ---------- Rainbow ----------

static void test_rainbow_does_not_fire_before_first_step_interval(void) {
    effects::Rainbow r("test-target", /*hz=*/1.0f, /*v=*/1.0f, /*step=*/50);
    r.enter();
    r.loop_tick(0);    // last_step_ms_ becomes 0; delta=0 - 0 = 0 < 50, no fire
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.count());
}

static void test_rainbow_fires_after_step_interval_elapses(void) {
    effects::Rainbow r("test-target", /*hz=*/1.0f, /*v=*/1.0f, /*step=*/50);
    r.enter();
    r.loop_tick(50);   // delta from 0 = 50 >= 50, fires
    TEST_ASSERT_EQUAL_INT(1, test_device::driver.count());
}

static void test_rainbow_fires_n_times_over_n_intervals(void) {
    effects::Rainbow r("test-target", /*hz=*/1.0f, /*v=*/1.0f, /*step=*/50);
    r.enter();
    for (uint32_t t = 0; t <= 500; t += 50) r.loop_tick(t);
    // Ticks at 0 (no), 50, 100, 150, 200, 250, 300, 350, 400, 450, 500 = 10 fires
    TEST_ASSERT_EQUAL_INT(10, test_device::driver.count());
}

static void test_hsv_to_rgb_primaries(void) {
    uint8_t r, g, b;
    effects::hsv_to_rgb(0.0f,   1.0f, 1.0f, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0xFF, r); TEST_ASSERT_EQUAL_UINT8(0x00, g); TEST_ASSERT_EQUAL_UINT8(0x00, b);
    effects::hsv_to_rgb(120.0f, 1.0f, 1.0f, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0x00, r); TEST_ASSERT_EQUAL_UINT8(0xFF, g); TEST_ASSERT_EQUAL_UINT8(0x00, b);
    effects::hsv_to_rgb(240.0f, 1.0f, 1.0f, r, g, b);
    TEST_ASSERT_EQUAL_UINT8(0x00, r); TEST_ASSERT_EQUAL_UINT8(0x00, g); TEST_ASSERT_EQUAL_UINT8(0xFF, b);
}

static void test_hsv_to_rgb_brightness_scales(void) {
    uint8_t r, g, b;
    effects::hsv_to_rgb(0.0f, 1.0f, 0.5f, r, g, b);
    TEST_ASSERT_UINT8_WITHIN(2, 127, r);    // 0xFF * 0.5 ~= 127
    TEST_ASSERT_EQUAL_UINT8(0x00, g);
    TEST_ASSERT_EQUAL_UINT8(0x00, b);
}

// ---------- Starlight ----------

static void test_starlight_first_tick_schedules_then_fires(void) {
    std::srand(1);
    effects::Starlight s("test-target", /*mean=*/100, /*jitter=*/0,
                         /*chance=*/pixmob::CHANCE_16);
    s.enter();
    // First tick at t=0 schedules next_fire = 0 + interval. With jitter=0,
    // interval is exactly mean (100). So loop_tick(0) does NOT fire.
    s.loop_tick(0);
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.count());

    // At t=99 still before scheduled time, no fire.
    s.loop_tick(99);
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.count());

    // At t=100, fires.
    s.loop_tick(100);
    TEST_ASSERT_EQUAL_INT(1, test_device::driver.count());
}

static void test_starlight_uses_configured_chance(void) {
    std::srand(1);
    effects::Starlight s("test-target", 50, 0, pixmob::CHANCE_50);
    s.enter();
    s.loop_tick(0);
    s.loop_tick(50);
    TEST_ASSERT_EQUAL_INT(1, test_device::driver.count());
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_50,
                          (int)test_device::driver.last().chance);
}

static void test_starlight_uses_long_release_for_fade(void) {
    std::srand(1);
    effects::Starlight s("test-target", 50, 0, pixmob::CHANCE_16);
    s.enter();
    s.loop_tick(0);
    s.loop_tick(50);
    auto ev = test_device::driver.last();
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)ev.release);
}

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_pulse_on_beat_fires_one_event_with_set_colour);
    RUN_TEST(test_pulse_envelope_picks_punchy_default_for_unknown_bpm);
    RUN_TEST(test_pulse_envelope_picks_fast_for_high_bpm);
    RUN_TEST(test_pulse_envelope_picks_slow_ballad_for_low_bpm);
    RUN_TEST(test_pulse_skips_fire_when_colour_is_off);

    RUN_TEST(test_probability_pulse_passes_chance_to_encoder);

    RUN_TEST(test_rainbow_does_not_fire_before_first_step_interval);
    RUN_TEST(test_rainbow_fires_after_step_interval_elapses);
    RUN_TEST(test_rainbow_fires_n_times_over_n_intervals);
    RUN_TEST(test_hsv_to_rgb_primaries);
    RUN_TEST(test_hsv_to_rgb_brightness_scales);

    RUN_TEST(test_starlight_first_tick_schedules_then_fires);
    RUN_TEST(test_starlight_uses_configured_chance);
    RUN_TEST(test_starlight_uses_long_release_for_fade);

    return UNITY_END();
}
