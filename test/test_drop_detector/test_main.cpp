// Native unit tests for DropDetector (Epic 4.5 Block 4a).
//
// Strategy: feed the detector a scalar bass-energy stream simulating
// song-structure transitions - steady verse, sustained chorus,
// breakdown, etc. - and assert it fires DROP / BREAKDOWN / None at
// the expected moments. Cooldown and ratio thresholds are exercised
// as separate cases so a tuning regression in one doesn't mask a
// bug in another.

#include "dal/analyser/drop_detector.h"

#include <unity.h>

using namespace nocturnation::dal::analyser;

namespace {

// Drive the detector for `frames` frames at `bass` energy starting at
// `start_ms`, advancing 25 ms per frame. Returns the number of fires
// and updates `now_ms` in place.
size_t feed_steady(DropDetector& det,
                   float    bass,
                   size_t   frames,
                   uint32_t& now_ms,
                   DropEvent expected_filter = DropEvent::None) {
    size_t fires = 0;
    for (size_t i = 0; i < frames; ++i) {
        const DropEvent e = det.process(bass, now_ms);
        if (e != DropEvent::None) ++fires;
        now_ms += 25;
        (void)expected_filter;
    }
    return fires;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Warm-up suppresses all fires until the long window is populated.
// ---------------------------------------------------------------------------

static void test_warmup_suppresses_initial_fires(void) {
    DropDetector det;
    uint32_t t = 0;

    // Even an immediate massive spike on frame 0 must not fire - the
    // warm-up window (default 80 frames) hasn't elapsed yet.
    const DropEvent first = det.process(1000.0f, t);
    TEST_ASSERT_TRUE(first == DropEvent::None);

    // Run quiet for 79 frames; should also stay None.
    size_t fires = feed_steady(det, 10.0f, 79, t);
    TEST_ASSERT_EQUAL_UINT(0u, fires);
}

// ---------------------------------------------------------------------------
// Steady-state energy doesn't fire either DROP or BREAKDOWN.
// ---------------------------------------------------------------------------

static void test_steady_energy_no_fires(void) {
    DropDetector det;
    uint32_t t = 0;
    const size_t fires = feed_steady(det, 100.0f, 500, t);
    TEST_ASSERT_EQUAL_UINT(0u, fires);
}

// ---------------------------------------------------------------------------
// Step up (verse → loud chorus) fires exactly one DROP.
// ---------------------------------------------------------------------------

static void test_step_up_fires_drop(void) {
    DropDetector det;
    uint32_t t = 0;

    // 10 seconds of low energy to populate the long window past warm-up.
    feed_steady(det, 10.0f, 400, t);

    // Now spike to 100 (10x the long-window mean - well above drop_ratio).
    int drops = 0;
    int breakdowns = 0;
    for (size_t i = 0; i < 100; ++i) {
        const DropEvent e = det.process(100.0f, t);
        if      (e == DropEvent::Drop)      ++drops;
        else if (e == DropEvent::Breakdown) ++breakdowns;
        t += 25;
    }
    TEST_ASSERT_EQUAL_INT(1, drops);          // exactly one DROP at the transition
    TEST_ASSERT_EQUAL_INT(0, breakdowns);     // no spurious BREAKDOWN
}

// ---------------------------------------------------------------------------
// Step down (loud → quiet breakdown) fires exactly one BREAKDOWN.
// ---------------------------------------------------------------------------

static void test_step_down_fires_breakdown(void) {
    DropDetector det;
    uint32_t t = 0;

    // 10 s of loud energy to set the baseline.
    feed_steady(det, 100.0f, 400, t);

    // Drop to 10 (0.1x long-window mean - below breakdown_ratio of 0.4).
    int drops = 0;
    int breakdowns = 0;
    for (size_t i = 0; i < 100; ++i) {
        const DropEvent e = det.process(10.0f, t);
        if      (e == DropEvent::Drop)      ++drops;
        else if (e == DropEvent::Breakdown) ++breakdowns;
        t += 25;
    }
    TEST_ASSERT_EQUAL_INT(0, drops);
    TEST_ASSERT_EQUAL_INT(1, breakdowns);
}

// ---------------------------------------------------------------------------
// Cooldown blocks rapid second fires after a DROP.
// ---------------------------------------------------------------------------

static void test_cooldown_blocks_repeat_fire(void) {
    DropDetector det;
    uint32_t t = 0;

    feed_steady(det, 10.0f, 400, t);

    // Run high energy until DROP fires. With short_window=80 frames,
    // the algorithm needs ~2 s of sustained high energy for the short
    // mean to climb above drop_ratio × long_mean.
    int drops = 0;
    uint32_t first_drop_ms = 0;
    for (size_t i = 0; i < 200; ++i) {
        if (det.process(100.0f, t) == DropEvent::Drop) {
            ++drops;
            if (first_drop_ms == 0) first_drop_ms = t;
        }
        t += 25;
    }
    // First DROP must fire exactly once during the high-energy run -
    // any subsequent fires within those 5 s would mean cooldown is
    // broken.
    TEST_ASSERT_EQUAL_INT(1, drops);
    TEST_ASSERT_GREATER_THAN_UINT(0, first_drop_ms);
}

// ---------------------------------------------------------------------------
// After cooldown elapses, a fresh transition can fire again.
// ---------------------------------------------------------------------------

static void test_post_cooldown_can_fire_again(void) {
    DropDetector det;
    uint32_t t = 0;

    // Set baseline at 10. Then run high energy to fire DROP.
    feed_steady(det, 10.0f, 400, t);
    bool got_drop = false;
    for (size_t i = 0; i < 200 && !got_drop; ++i) {
        if (det.process(100.0f, t) == DropEvent::Drop) got_drop = true;
        t += 25;
    }
    TEST_ASSERT_TRUE(got_drop);

    // Run 12 s of very low energy (below baseline). The cooldown
    // elapses and the long-window mean stays inflated by recent
    // high-energy history while the short-window dives - that's the
    // ratio signature for BREAKDOWN.
    int breakdowns = 0;
    for (size_t i = 0; i < 480; ++i) {
        if (det.process(2.0f, t) == DropEvent::Breakdown) ++breakdowns;
        t += 25;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, breakdowns);
}

// ---------------------------------------------------------------------------
// Modest energy swing does NOT fire (verse-to-bridge swell shouldn't
// trip the detector).
// ---------------------------------------------------------------------------

static void test_modest_swing_no_fire(void) {
    DropDetector det;
    uint32_t t = 0;

    feed_steady(det, 50.0f, 400, t);  // baseline 50

    // Rise to 70 - a 1.4x swing, below the 1.8x drop threshold.
    int fires = 0;
    for (size_t i = 0; i < 100; ++i) {
        if (det.process(70.0f, t) != DropEvent::None) ++fires;
        t += 25;
    }
    TEST_ASSERT_EQUAL_INT(0, fires);
}

// ---------------------------------------------------------------------------
// All-zero history doesn't crash. Documents the div-by-zero guard:
// during pure silence the long-window mean is exactly 0, ratio is
// undefined, and the guard returns None for that frame. Realistic
// audio has a noise floor so this is a synthetic edge case rather
// than a steady-state condition; the test exists to verify the guard
// rather than a behavioural property.
// ---------------------------------------------------------------------------

static void test_silent_history_safe(void) {
    DropDetector det;
    uint32_t t = 0;

    // 500 frames of zero input. Each frame: l_mean = 0, guard
    // returns None. No crash, no fire.
    int fires = 0;
    for (size_t i = 0; i < 500; ++i) {
        if (det.process(0.0f, t) != DropEvent::None) ++fires;
        t += 25;
    }
    TEST_ASSERT_EQUAL_INT(0, fires);
}

// ---------------------------------------------------------------------------
// Tunable thresholds: tighter drop_ratio suppresses borderline drops.
// ---------------------------------------------------------------------------

static void test_tighter_drop_ratio_suppresses_borderline(void) {
    // After 80 frames of high energy at 19 against a 10-baseline, the
    // short-mean is 19 and the long-mean is (320×10 + 80×19)/400 =
    // 11.8, giving a ratio of ~1.61. Loose drop_ratio (1.5) should
    // fire there; strict (1.8) should not.
    DropDetectorConfig strict_cfg;
    strict_cfg.drop_ratio = 1.8f;
    DropDetector strict(strict_cfg);

    DropDetectorConfig loose_cfg;
    loose_cfg.drop_ratio = 1.5f;
    DropDetector loose(loose_cfg);

    uint32_t t_strict = 0;
    feed_steady(strict, 10.0f, 400, t_strict);
    int strict_fires = 0;
    for (size_t i = 0; i < 100; ++i) {
        if (strict.process(19.0f, t_strict) != DropEvent::None) ++strict_fires;
        t_strict += 25;
    }
    TEST_ASSERT_EQUAL_INT(0, strict_fires);

    uint32_t t_loose = 0;
    feed_steady(loose, 10.0f, 400, t_loose);
    int loose_fires = 0;
    for (size_t i = 0; i < 200; ++i) {
        if (loose.process(19.0f, t_loose) != DropEvent::None) ++loose_fires;
        t_loose += 25;
    }
    TEST_ASSERT_GREATER_OR_EQUAL_INT(1, loose_fires);
}

// ---------------------------------------------------------------------------
// reset() clears state.
// ---------------------------------------------------------------------------

static void test_reset_clears_state(void) {
    DropDetector det;
    uint32_t t = 0;

    // Run baseline + sustained high energy until DROP fires to set
    // the cooldown clock and populate history.
    feed_steady(det, 10.0f, 400, t);
    bool got_drop = false;
    for (size_t i = 0; i < 200 && !got_drop; ++i) {
        if (det.process(100.0f, t) == DropEvent::Drop) got_drop = true;
        t += 25;
    }
    TEST_ASSERT_TRUE(got_drop);

    det.reset();

    // Post-reset, frames_seen and last_event_ms are zeroed.
    TEST_ASSERT_EQUAL_UINT(0u, det.frames_seen());
    TEST_ASSERT_EQUAL_UINT(0u, det.last_event_ms());

    // And the warm-up restarts: 79 frames at any energy don't fire.
    int fires = 0;
    for (size_t i = 0; i < 79; ++i) {
        if (det.process(100.0f, t) != DropEvent::None) ++fires;
        t += 25;
    }
    TEST_ASSERT_EQUAL_INT(0, fires);
}

// ---------------------------------------------------------------------------
// Wire-format compatibility: enum values match architecture spec §4.3.
// MUSIC_EVENT payload byte: 1 = DROP, 2 = BREAKDOWN, 3 = BUILD reserved.
// 0 = "no event this frame", a local convention not on the wire.
// ---------------------------------------------------------------------------

static void test_enum_values_match_wire_protocol(void) {
    TEST_ASSERT_EQUAL_UINT(0, static_cast<uint8_t>(DropEvent::None));
    TEST_ASSERT_EQUAL_UINT(1, static_cast<uint8_t>(DropEvent::Drop));
    TEST_ASSERT_EQUAL_UINT(2, static_cast<uint8_t>(DropEvent::Breakdown));
    TEST_ASSERT_EQUAL_UINT(3, static_cast<uint8_t>(DropEvent::Build));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_warmup_suppresses_initial_fires);
    RUN_TEST(test_steady_energy_no_fires);
    RUN_TEST(test_step_up_fires_drop);
    RUN_TEST(test_step_down_fires_breakdown);
    RUN_TEST(test_cooldown_blocks_repeat_fire);
    RUN_TEST(test_post_cooldown_can_fire_again);
    RUN_TEST(test_modest_swing_no_fire);
    RUN_TEST(test_silent_history_safe);
    RUN_TEST(test_tighter_drop_ratio_suppresses_borderline);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_enum_values_match_wire_protocol);

    return UNITY_END();
}
