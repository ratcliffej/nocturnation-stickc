// Native unit tests for SectionDetector (Epic 4.7 Block 4).
//
// Strategy: drive the detector with synthetic descriptor sequences
// (centroid / energy / density u8s plus drop_event_fired flags) over
// many frames and assert the section label tracks the expected
// transitions. No FFT, HAL, or DAL dependency - the detector consumes
// the same u8 surface MusicDescriptors produces.

#include "dal/analyser/section_detector.h"

#include <unity.h>

using namespace nocturnation::dal::analyser;

namespace {

// Pump `frames` updates with constant (c, e, d) at fixed cadence.
SectionType pump_steady(SectionDetector& det,
                         uint8_t c, uint8_t e, uint8_t d,
                         size_t frames) {
    SectionType last = SectionType::Unknown;
    for (size_t i = 0; i < frames; ++i) {
        last = det.process(c, e, d, /*drop=*/false);
    }
    return last;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Warm-up: detector stays UNKNOWN until warmup_frames elapse.
// ---------------------------------------------------------------------------

static void test_warmup_keeps_unknown(void) {
    SectionDetector det;
    // Hammer a "chorus-like" profile during warm-up.
    for (size_t i = 0; i < 40; ++i) {   // < 80 warmup_frames
        det.process(/*c=*/180, /*e=*/200, /*d=*/180, false);
    }
    TEST_ASSERT_EQUAL_INT((int)SectionType::Unknown, (int)det.current());
}

// ---------------------------------------------------------------------------
// CHORUS: sustained high energy + high density, after warm-up + hysteresis.
// ---------------------------------------------------------------------------

static void test_chorus_after_sustained_high_levels(void) {
    SectionDetector det;
    const SectionType end = pump_steady(det,
        /*c=*/180, /*e=*/200, /*d=*/180,
        /*frames=*/400);   // well past warmup + hysteresis
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)end);
}

// ---------------------------------------------------------------------------
// VERSE: mid energy, mid density, after warm-up + hysteresis.
// ---------------------------------------------------------------------------

static void test_verse_after_sustained_mid_levels(void) {
    SectionDetector det;
    const SectionType end = pump_steady(det,
        /*c=*/120, /*e=*/120, /*d=*/60,
        /*frames=*/400);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Verse, (int)end);
}

// ---------------------------------------------------------------------------
// BREAKDOWN: sustained low energy + low density.
// ---------------------------------------------------------------------------

static void test_breakdown_after_sustained_low_levels(void) {
    SectionDetector det;
    // Warm up to chorus first so the slow IIR has a non-zero baseline,
    // then drop to silence and verify BREAKDOWN latches after the
    // sustain check fills.
    pump_steady(det, 180, 200, 180, 400);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());

    // 200 frames of low energy + low density (~5 s @ 40 Hz).
    pump_steady(det, /*c=*/100, /*e=*/16, /*d=*/8, /*frames=*/200);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Breakdown, (int)det.current());
}

// ---------------------------------------------------------------------------
// BUILDUP: rising energy + density slopes detected over the window.
// ---------------------------------------------------------------------------

static void test_buildup_during_rising_slopes(void) {
    SectionDetector det;
    // Warm up on a low-mid steady profile first.
    pump_steady(det, 120, 80, 40, 150);

    // Now ramp energy + density up linearly over ~100 frames.
    SectionType last = SectionType::Unknown;
    for (size_t i = 0; i < 100; ++i) {
        const float t = static_cast<float>(i) / 100.0f;
        const uint8_t e = static_cast<uint8_t>(80.0f + t * 150.0f);
        const uint8_t d = static_cast<uint8_t>(40.0f + t * 120.0f);
        last = det.process(120, e, d, false);
    }
    // At some point during the ramp BUILDUP should have latched.
    TEST_ASSERT_EQUAL_INT((int)SectionType::BuildUp, (int)last);
}

// ---------------------------------------------------------------------------
// DROP: external event latches DROP for drop_hold_frames then releases.
// ---------------------------------------------------------------------------

static void test_drop_event_latches_and_releases(void) {
    SectionDetector det;
    // Warm-up on a chorus profile so the post-DROP state is well-defined.
    pump_steady(det, 180, 200, 180, 400);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());

    // Fire a DROP event - section latches to DROP.
    det.process(180, 200, 180, /*drop=*/true);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Drop, (int)det.current());

    // Default drop_hold_frames = 40; stays DROP for that window.
    for (size_t i = 0; i < 30; ++i) {
        det.process(180, 200, 180, false);
    }
    TEST_ASSERT_EQUAL_INT((int)SectionType::Drop, (int)det.current());

    // After the hold expires + hysteresis, CHORUS resumes.
    pump_steady(det, 180, 200, 180, 100);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());
}

// ---------------------------------------------------------------------------
// Hysteresis: a brief dip doesn't bounce the detector out of CHORUS.
// ---------------------------------------------------------------------------

static void test_hysteresis_resists_brief_dip(void) {
    SectionDetector det;
    pump_steady(det, 180, 200, 180, 400);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());

    // 4 frames of low values - shorter than transition_frames (8).
    for (size_t i = 0; i < 4; ++i) {
        det.process(100, 40, 20, false);
    }
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());
}

// ---------------------------------------------------------------------------
// reset() returns to UNKNOWN + clears IIR state.
// ---------------------------------------------------------------------------

static void test_reset_returns_to_unknown(void) {
    SectionDetector det;
    pump_steady(det, 180, 200, 180, 400);
    TEST_ASSERT_EQUAL_INT((int)SectionType::Chorus, (int)det.current());

    det.reset();
    TEST_ASSERT_EQUAL_INT((int)SectionType::Unknown, (int)det.current());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_warmup_keeps_unknown);
    RUN_TEST(test_chorus_after_sustained_high_levels);
    RUN_TEST(test_verse_after_sustained_mid_levels);
    RUN_TEST(test_breakdown_after_sustained_low_levels);
    RUN_TEST(test_buildup_during_rising_slopes);
    RUN_TEST(test_drop_event_latches_and_releases);
    RUN_TEST(test_hysteresis_resists_brief_dip);
    RUN_TEST(test_reset_returns_to_unknown);
    return UNITY_END();
}
