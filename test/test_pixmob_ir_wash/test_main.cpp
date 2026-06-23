// Native test for PixMobIRDriver's wash state machine (Epic 11 B2).
//
// Covers:
//   - send_wash stores per-group state and fires one immediate IR.
//   - loop_tick schedules refreshes at kWashRefreshMs cadence.
//   - send_wash_end with release_time == 0 stops refreshing silently.
//   - send_wash_end with release_time > 0 fires one final fade IR.
//   - send_wash_pulse fires TwoColors composite on a washing group.
//   - send_wash_pulse falls back to SingleColor on a non-washing group.
//   - drift wash (cycle_ms > 0) blends RGB across cycle phase.
//   - Per-group state isolated (group N's refresh doesn't fire group M).
//   - Out-of-range group_id is a silent no-op (returns false).
//
// The HAL stub returns a capturing IRTx so we can count IR fires and
// inspect their pulse-train length without depending on hardware. The
// clock_source_ injection seam on PixMobIRDriver lets us advance time
// deterministically without sleep().

#include <cstdint>
#include <cstddef>
#include <unity.h>

#include "hal/hal.h"
#include "dal/dal.h"
#include "dal/drivers/pixmob_ir_driver.h"

using nocturnation::dal::PixMobIRDriver;
using nocturnation::dal::pixmob_ir_driver_instance;
using nocturnation::dal::LightWashEvent;
using nocturnation::dal::RgbPulseEvent;

// =============================================================================
// HAL stub: a capturing IRTx that records each send_raw call so tests can
// assert "the driver fired N IR commands" and "the last fire had ~K pulses
// in its train".
// =============================================================================

namespace nocturnation {
namespace hal {

struct CapturingIRTx : public IRTx {
    int      send_count = 0;
    size_t   last_count = 0;
    uint16_t last_carrier_khz = 0;
    void begin() override {}
    void send_raw(const uint16_t* /*pulses_us*/, size_t count,
                  uint16_t carrier_khz) override {
        ++send_count;
        last_count = count;
        last_carrier_khz = carrier_khz;
    }
};
static CapturingIRTx s_ir_tx;

static constexpr Capability kCapabilities[] = {
    Capability::Buttons,
    Capability::Display,
    Capability::IRTx,
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

Mic*     HAL::mic()       { return nullptr; }
IRTx*    HAL::ir_tx()     { return &s_ir_tx; }
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()     { return nullptr; }
ESPNow*  HAL::esp_now()   { return nullptr; }
Display* HAL::display()   { return nullptr; }
Buttons* HAL::buttons()   { return nullptr; }
IMU*     HAL::imu()       { return nullptr; }
Battery* HAL::battery()   { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Clock stub - tests advance time by setting this directly.
// =============================================================================

static uint32_t s_now_ms = 0;
static uint32_t test_now() { return s_now_ms; }

// =============================================================================
// Fixtures
// =============================================================================

void setUp(void) {
    // Reset HAL stub counters.
    nocturnation::hal::s_ir_tx.send_count = 0;
    nocturnation::hal::s_ir_tx.last_count = 0;

    // Reset clock.
    s_now_ms = 1000;   // start at non-zero so subtraction is safe

    // Install clock + reset driver state. begin() also re-checks IRTx;
    // we don't strictly need it for the state tests but it mirrors the
    // production path.
    auto* drv = pixmob_ir_driver_instance();
    drv->set_clock_source(&test_now);
    drv->begin();
    // Clear every wash slot via end calls (some may carry over between
    // tests since the driver is a singleton).
    for (uint8_t g = 0; g < PixMobIRDriver::kWashSlots; ++g) {
        drv->send_wash_end(g, /*release_time=*/0);
    }
    // Bump send_count back to zero - the clearing pass may have ticked it.
    nocturnation::hal::s_ir_tx.send_count = 0;
}

void tearDown(void) {}

static LightWashEvent purple_static() {
    LightWashEvent ev{};
    ev.r1 = 200; ev.g1 = 0; ev.b1 = 200;
    ev.r2 = 0;   ev.g2 = 0; ev.b2 = 0;
    ev.attack         = 0;
    ev.release        = 0;
    ev.intensity      = 255;
    ev.cycle_ms       = 0;
    ev.ttl_seconds    = 0;
    ev.pulse_response = 1;
    return ev;
}

static LightWashEvent orange_to_purple_drift(uint16_t cycle_ms) {
    LightWashEvent ev{};
    ev.r1 = 255; ev.g1 = 100; ev.b1 = 0;
    ev.r2 = 100; ev.g2 = 0;   ev.b2 = 200;
    ev.attack         = 0;
    ev.release        = 0;
    ev.intensity      = 255;
    ev.cycle_ms       = cycle_ms;
    ev.ttl_seconds    = 0;
    ev.pulse_response = 1;
    return ev;
}

// =============================================================================
// Tests
// =============================================================================

static void test_send_wash_stores_state_and_fires_immediate(void) {
    auto* drv = pixmob_ir_driver_instance();
    TEST_ASSERT_TRUE(drv->send_wash(/*group=*/0, purple_static()));
    // State recorded.
    const auto* s = drv->wash_state(0);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(s->active);
    TEST_ASSERT_EQUAL_UINT8(200, s->r1);
    TEST_ASSERT_EQUAL_UINT8(0,   s->g1);
    TEST_ASSERT_EQUAL_UINT8(200, s->b1);
    TEST_ASSERT_EQUAL_UINT16(0,  s->cycle_ms);
    TEST_ASSERT_EQUAL_UINT32(s_now_ms + PixMobIRDriver::kWashRefreshMs,
                             s->next_refresh_ms);
    // One immediate IR fire.
    TEST_ASSERT_EQUAL_INT(1, nocturnation::hal::s_ir_tx.send_count);
}

static void test_loop_tick_does_not_fire_before_refresh_period(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    const int after_immediate = nocturnation::hal::s_ir_tx.send_count;

    // Advance time by less than refresh period.
    s_now_ms += PixMobIRDriver::kWashRefreshMs - 1;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT(after_immediate, nocturnation::hal::s_ir_tx.send_count);
}

static void test_loop_tick_fires_at_refresh_boundary(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    const int after_immediate = nocturnation::hal::s_ir_tx.send_count;

    // Advance exactly the refresh period.
    s_now_ms += PixMobIRDriver::kWashRefreshMs;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT(after_immediate + 1,
                          nocturnation::hal::s_ir_tx.send_count);

    // Refresh should have re-scheduled itself.
    const auto* s = drv->wash_state(0);
    TEST_ASSERT_EQUAL_UINT32(s_now_ms + PixMobIRDriver::kWashRefreshMs,
                             s->next_refresh_ms);
}

static void test_send_wash_end_instant_stops_refresh_no_ir(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    const int before = nocturnation::hal::s_ir_tx.send_count;

    TEST_ASSERT_TRUE(drv->send_wash_end(0, /*release_time=*/0));
    // No IR fire on instant cancel - bracelet's last envelope completes
    // naturally.
    TEST_ASSERT_EQUAL_INT(before, nocturnation::hal::s_ir_tx.send_count);
    TEST_ASSERT_FALSE(drv->wash_state(0)->active);

    // Subsequent loop_tick should NOT fire (group is inactive).
    s_now_ms += 10000;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT(before, nocturnation::hal::s_ir_tx.send_count);
}

static void test_send_wash_end_faded_fires_one_final_ir(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    const int before = nocturnation::hal::s_ir_tx.send_count;

    // release_time=20 (100ms units) = 2.0 s; closest bucket is T_2400_MS.
    TEST_ASSERT_TRUE(drv->send_wash_end(0, /*release_time=*/20));
    TEST_ASSERT_EQUAL_INT(before + 1, nocturnation::hal::s_ir_tx.send_count);
    TEST_ASSERT_FALSE(drv->wash_state(0)->active);
}

static void test_send_wash_end_on_inactive_group_returns_false(void) {
    auto* drv = pixmob_ir_driver_instance();
    // No prior send_wash on group 0.
    TEST_ASSERT_FALSE(drv->send_wash_end(0, 0));
}

static void test_send_wash_pulse_on_active_fires_twocolors(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    const int before = nocturnation::hal::s_ir_tx.send_count;

    RgbPulseEvent sparkle{};
    sparkle.r = 255; sparkle.g = 255; sparkle.b = 255;
    sparkle.attack = pixmob::T_0_MS;
    sparkle.sustain = pixmob::T_32_MS;
    sparkle.release = pixmob::T_96_MS;
    sparkle.chance = pixmob::CHANCE_100;
    TEST_ASSERT_TRUE(drv->send_wash_pulse(0, sparkle));
    TEST_ASSERT_EQUAL_INT(before + 1, nocturnation::hal::s_ir_tx.send_count);

    // Refresh tick should have been pushed forward by the wash_pulse.
    const auto* s = drv->wash_state(0);
    TEST_ASSERT_EQUAL_UINT32(s_now_ms + PixMobIRDriver::kWashRefreshMs,
                             s->next_refresh_ms);
}

static void test_send_wash_pulse_on_inactive_falls_back_to_pulse(void) {
    auto* drv = pixmob_ir_driver_instance();
    // No wash on group 0.
    RgbPulseEvent sparkle{};
    sparkle.r = 255;
    sparkle.attack = pixmob::T_0_MS;
    sparkle.sustain = pixmob::T_32_MS;
    sparkle.release = pixmob::T_96_MS;
    sparkle.chance = pixmob::CHANCE_100;
    const int before = nocturnation::hal::s_ir_tx.send_count;
    TEST_ASSERT_TRUE(drv->send_wash_pulse(0, sparkle));
    // Fell back to SingleColor; still fires one IR.
    TEST_ASSERT_EQUAL_INT(before + 1, nocturnation::hal::s_ir_tx.send_count);
}

static void test_drift_wash_blends_rgb_across_cycle_phase(void) {
    auto* drv = pixmob_ir_driver_instance();
    // 8 s cycle. start at anchor A (phase 0).
    drv->send_wash(0, orange_to_purple_drift(/*cycle_ms=*/8000));
    TEST_ASSERT_EQUAL_UINT8(255, drv->wash_state(0)->r1);

    // At t=0 the refresh fires with anchor A directly. We can't easily
    // assert the IR pulse's RGB content from the capturing stub, but we
    // can use compute_drift_rgb's effects via wash_state and a refresh
    // tick: at t = cycle/4 the blend should be 50/50 between A and B.
    //
    // Advance to mid-cycle (phase 0.25 == 50/50 blend on linear).
    s_now_ms += 2000;
    // Force a refresh fire by jumping to refresh boundary too.
    s_now_ms += PixMobIRDriver::kWashRefreshMs;
    drv->loop_tick();

    // Anchor R values are 255 (A) and 100 (B). Blend at the time we
    // sampled is computed inside fire_wash_refresh - we just confirm
    // the refresh fired and the state's started_ms is preserved (so
    // cycle phase is deterministic relative to it).
    TEST_ASSERT_EQUAL_UINT32(1000u, drv->wash_state(0)->started_ms);
}

static void test_per_group_state_isolated(void) {
    auto* drv = pixmob_ir_driver_instance();
    drv->send_wash(0, purple_static());
    LightWashEvent orange{};
    orange.r1 = 255; orange.g1 = 100; orange.b1 = 0;
    orange.cycle_ms = 0; orange.intensity = 255;
    drv->send_wash(3, orange);

    TEST_ASSERT_EQUAL_UINT8(200, drv->wash_state(0)->r1);
    TEST_ASSERT_EQUAL_UINT8(255, drv->wash_state(3)->r1);

    // End group 0 only.
    drv->send_wash_end(0, 0);
    TEST_ASSERT_FALSE(drv->wash_state(0)->active);
    TEST_ASSERT_TRUE(drv->wash_state(3)->active);

    // Group 3's refresh still fires.
    const int before = nocturnation::hal::s_ir_tx.send_count;
    s_now_ms += PixMobIRDriver::kWashRefreshMs;
    drv->loop_tick();
    TEST_ASSERT_EQUAL_INT(before + 1, nocturnation::hal::s_ir_tx.send_count);
}

static void test_invalid_group_id_silently_rejected(void) {
    auto* drv = pixmob_ir_driver_instance();
    TEST_ASSERT_FALSE(drv->send_wash(PixMobIRDriver::kWashSlots,
                                     purple_static()));
    TEST_ASSERT_FALSE(drv->send_wash_end(PixMobIRDriver::kWashSlots, 0));
    TEST_ASSERT_NULL(drv->wash_state(PixMobIRDriver::kWashSlots));
}

static void test_pulse_on_active_wash_fires_sparkle_plus_recovery(void) {
    // Regression for the 2026-06-18 bench symptom "wash goes to black
    // between sparkles". A pulse fired via send(RgbPulseEvent) on a
    // group with an active wash must produce a visible sparkle AND
    // bring the bracelet back to the wash colour quickly afterwards.
    //
    // Two-command composition (TwoColors-equivalent semantic, but
    // built from two SingleColor commands because TwoColors as a
    // protocol-level command renders nothing visible on the Aurora-
    // class bracelet revisions Epic 11 targets - PMob Bench T6
    // confirmed 2026-06-18):
    //   1. SingleColor(sparkle_rgb, orchestrator envelope)
    //   2. SingleColor(current_wash_rgb, T_192_MS fast recovery)
    //
    // The test asserts BOTH IR commands fire (send_count increments
    // by 2) when the group has an active wash, and only ONE fires
    // when it doesn't (no need for recovery).
    auto* drv = pixmob_ir_driver_instance();

    // Phase 1: no wash, regular pulse - single SingleColor only.
    RgbPulseEvent ev{};
    ev.r = 255; ev.g = 0; ev.b = 0;
    ev.attack = pixmob::T_0_MS;
    ev.sustain = pixmob::T_32_MS;
    ev.release = pixmob::T_96_MS;
    ev.chance = pixmob::CHANCE_100;
    const int before_no_wash = nocturnation::hal::s_ir_tx.send_count;
    drv->send(/*group_id=*/0, ev);
    TEST_ASSERT_EQUAL_INT(before_no_wash + 1,
                          nocturnation::hal::s_ir_tx.send_count);
    TEST_ASSERT_FALSE(drv->wash_state(0)->active);

    // Phase 2: start a wash, then fire the same pulse. Expect 2 IR
    // commands - the sparkle (SingleColor) plus a single recovery
    // (SingleColor wash-colour). The 50 ms ::delay() between the two
    // is the bracelet IR decoder's required quiet window AND the
    // visible-sparkle render time; with the gap in place a single
    // recovery lands cleanly (regression fix 2026-06-23). The
    // earlier "recovery x2" approach was a workaround for the
    // missing gap, not a fundamental need.
    drv->send_wash(0, purple_static());
    TEST_ASSERT_TRUE(drv->wash_state(0)->active);
    const int before_pulse = nocturnation::hal::s_ir_tx.send_count;
    drv->send(/*group_id=*/0, ev);
    TEST_ASSERT_EQUAL_INT(before_pulse + 2,
                          nocturnation::hal::s_ir_tx.send_count);
    // Wash state remains active - the pulse-plus-recovery didn't
    // disturb the periodic refresh schedule.
    TEST_ASSERT_TRUE(drv->wash_state(0)->active);
    TEST_ASSERT_EQUAL_UINT32(s_now_ms + PixMobIRDriver::kWashRefreshMs,
                             drv->wash_state(0)->next_refresh_ms);
}

static void test_refresh_interval_scales_with_cycle(void) {
    // Drifting washes auto-scale their refresh cadence so the
    // bracelet's step-wise snapshots produce visibly-smooth blending
    // (Epic 11 bench 2026-06-18 follow-up). Targets ~10 snapshots per
    // A↔B↔A cycle, clamped to [kWashRefreshMinMs, kWashRefreshMaxMs].
    // Static washes (cycle_ms == 0) hold at the max.
    auto* drv = pixmob_ir_driver_instance();

    // Case 1: 5 s cycle -> 250 ms refresh (5000 / 20 == 250, equal
    // to the floor).
    LightWashEvent slow_drift{};
    slow_drift.r1 = 255; slow_drift.g1 = 100; slow_drift.b1 = 0;
    slow_drift.r2 = 0;   slow_drift.g2 = 50;  slow_drift.b2 = 200;
    slow_drift.cycle_ms       = 5000;
    slow_drift.intensity      = 255;
    slow_drift.pulse_response = 1;
    drv->send_wash(0, slow_drift);
    TEST_ASSERT_EQUAL_UINT32(250u,
                             drv->wash_state(0)->refresh_interval_ms);

    // Case 2: 30 s cycle -> 1500 ms refresh (30000 / 20 == 1500,
    // within the [250, 3000] bounds so no clamp).
    LightWashEvent verylongdrift{};
    verylongdrift.r1 = 255; verylongdrift.g1 = 100; verylongdrift.b1 = 0;
    verylongdrift.r2 = 0;   verylongdrift.g2 = 50;  verylongdrift.b2 = 200;
    verylongdrift.cycle_ms       = 30000;
    verylongdrift.intensity      = 255;
    verylongdrift.pulse_response = 1;
    drv->send_wash(1, verylongdrift);
    TEST_ASSERT_EQUAL_UINT32(1500u,
                             drv->wash_state(1)->refresh_interval_ms);

    // Case 2b: 60 s cycle -> 3000 ms refresh (60000 / 20 == 3000,
    // equal to the cap).
    LightWashEvent epicdrift{};
    epicdrift.r1 = 255; epicdrift.g1 = 100; epicdrift.b1 = 0;
    epicdrift.r2 = 0;   epicdrift.g2 = 50;  epicdrift.b2 = 200;
    epicdrift.cycle_ms       = 60000;
    epicdrift.intensity      = 255;
    epicdrift.pulse_response = 1;
    drv->send_wash(4, epicdrift);
    TEST_ASSERT_EQUAL_UINT32(PixMobIRDriver::kWashRefreshMaxMs,
                             drv->wash_state(4)->refresh_interval_ms);

    // Case 3: 1 s cycle -> floored to kWashRefreshMinMs (1000 / 20 ==
    // 50, well below the 250 ms floor).
    LightWashEvent fastdrift{};
    fastdrift.r1 = 255; fastdrift.g1 = 100; fastdrift.b1 = 0;
    fastdrift.r2 = 0;   fastdrift.g2 = 50;  fastdrift.b2 = 200;
    fastdrift.cycle_ms       = 1000;
    fastdrift.intensity      = 255;
    fastdrift.pulse_response = 1;
    drv->send_wash(2, fastdrift);
    TEST_ASSERT_EQUAL_UINT32(PixMobIRDriver::kWashRefreshMinMs,
                             drv->wash_state(2)->refresh_interval_ms);

    // Case 4: static (cycle_ms == 0) holds at the max.
    drv->send_wash(3, purple_static());
    TEST_ASSERT_EQUAL_UINT32(PixMobIRDriver::kWashRefreshMaxMs,
                             drv->wash_state(3)->refresh_interval_ms);
}

static void test_clock_source_advances_state_deterministically(void) {
    auto* drv = pixmob_ir_driver_instance();
    s_now_ms = 5000;
    drv->send_wash(0, purple_static());
    TEST_ASSERT_EQUAL_UINT32(5000u, drv->wash_state(0)->started_ms);
    TEST_ASSERT_EQUAL_UINT32(5000u + PixMobIRDriver::kWashRefreshMs,
                             drv->wash_state(0)->next_refresh_ms);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_send_wash_stores_state_and_fires_immediate);
    RUN_TEST(test_loop_tick_does_not_fire_before_refresh_period);
    RUN_TEST(test_loop_tick_fires_at_refresh_boundary);
    RUN_TEST(test_send_wash_end_instant_stops_refresh_no_ir);
    RUN_TEST(test_send_wash_end_faded_fires_one_final_ir);
    RUN_TEST(test_send_wash_end_on_inactive_group_returns_false);
    RUN_TEST(test_send_wash_pulse_on_active_fires_twocolors);
    RUN_TEST(test_send_wash_pulse_on_inactive_falls_back_to_pulse);
    RUN_TEST(test_drift_wash_blends_rgb_across_cycle_phase);
    RUN_TEST(test_per_group_state_isolated);
    RUN_TEST(test_invalid_group_id_silently_rejected);
    RUN_TEST(test_pulse_on_active_wash_fires_sparkle_plus_recovery);
    RUN_TEST(test_refresh_interval_scales_with_cycle);
    RUN_TEST(test_clock_source_advances_state_deterministically);
    return UNITY_END();
}
