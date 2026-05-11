// Native unit tests for BeatDetector (Epic 4.5 Block 3a).
//
// Strategy: feed the detector synthetic spectrum frames - quiet
// background interrupted by spikes simulating kick onsets - and assert
// the detector fires at the right moments and stays quiet when it
// should. This isolates the threshold-and-refractory logic from FFT
// correctness; the spectrum frame is constructed by hand for each
// test scenario.

#include "dal/analyser/beat_detector.h"

#include <unity.h>

#include <cstring>

using namespace nocturnation::dal::analyser;

namespace {

// Helpers to build SpectrumFrame inputs concisely.

void zero_frame(SpectrumFrame& f) {
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = 0.0f;
}

void quiet_noise_frame(SpectrumFrame& f, float floor = 1.0f) {
    // Uniform low-level magnitude across all bands. Models room-tone
    // spectral noise floor: detector should treat this as steady
    // background and NOT fire.
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = floor;
}

void kick_frame(SpectrumFrame& f, float floor = 1.0f, float kick_peak = 50.0f) {
    // Quiet noise across all bands plus a strong spike in the bass
    // region (bands 0-10 cover ~30-215 Hz). Models a kick-drum onset.
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = floor;
    for (size_t i = 0; i < 11; ++i) f.magnitudes[i] = kick_peak;
}

void treble_spike_frame(SpectrumFrame& f, float floor = 1.0f, float spike = 50.0f) {
    // Quiet bass + a strong spike in the treble region (bands 22+,
    // ~5+ kHz). Models a hi-hat-like onset. The default detector
    // watches only the bass region (bands 0-10) so this should NOT
    // fire a kick beat.
    for (size_t i = 0; i < kSpectrumBands; ++i) f.magnitudes[i] = floor;
    for (size_t i = 22; i < kSpectrumBands; ++i) f.magnitudes[i] = spike;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Warm-up: the detector must NOT fire before its history is sufficiently
// populated.
// ---------------------------------------------------------------------------

static void test_does_not_fire_during_warmup(void) {
    BeatDetector det;
    SpectrumFrame f;
    kick_frame(f);  // immediate "kick" on frame 0

    // Default warmup_frames = 8; first 8 frames must not fire even
    // if the magnitudes look like a beat.
    for (uint32_t t = 0; t < 8; ++t) {
        const bool fired = det.process(f, t * 25);
        TEST_ASSERT_FALSE(fired);
    }
}

static void test_warmup_does_not_pollute_baseline(void) {
    // After the warm-up period elapses on a steady stream, a sudden
    // spike should still fire a beat. Verifies the warm-up window
    // didn't corrupt the variance computation by writing kick-shaped
    // history while suppressing fires.
    BeatDetector det;
    SpectrumFrame quiet;
    quiet_noise_frame(quiet, 1.0f);

    // Run quiet for many frames past warm-up.
    for (uint32_t t = 0; t < 20; ++t) {
        TEST_ASSERT_FALSE(det.process(quiet, t * 25));
    }

    // Now a kick - should fire.
    SpectrumFrame kick;
    kick_frame(kick, 1.0f, 50.0f);
    TEST_ASSERT_TRUE(det.process(kick, 21 * 25));
}

// ---------------------------------------------------------------------------
// Steady noise should not fire false beats.
// ---------------------------------------------------------------------------

static void test_steady_noise_does_not_fire(void) {
    BeatDetector det;
    SpectrumFrame f;
    quiet_noise_frame(f, 5.0f);

    int fires = 0;
    for (uint32_t t = 0; t < 200; ++t) {
        if (det.process(f, t * 25)) ++fires;
    }
    TEST_ASSERT_EQUAL_INT(0, fires);
}

// ---------------------------------------------------------------------------
// A clear spike against quiet background fires exactly once.
// ---------------------------------------------------------------------------

static void test_single_kick_fires_once(void) {
    BeatDetector det;
    SpectrumFrame quiet, kick;
    quiet_noise_frame(quiet, 1.0f);
    kick_frame(kick, 1.0f, 50.0f);

    // 20 frames of quiet to populate history past warm-up.
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    int fires = 0;
    if (det.process(kick, 20 * 25)) ++fires;

    // Subsequent quiet frames should not fire (kick magnitude is gone).
    for (uint32_t t = 21; t < 30; ++t) {
        if (det.process(quiet, t * 25)) ++fires;
    }
    TEST_ASSERT_EQUAL_INT(1, fires);
}

// ---------------------------------------------------------------------------
// Refractory period blocks rapid double-fires.
// ---------------------------------------------------------------------------

static void test_refractory_blocks_consecutive_kicks(void) {
    BeatDetector::Config cfg;
    cfg.refractory_ms = 100;
    BeatDetector det(cfg);

    SpectrumFrame quiet, kick;
    quiet_noise_frame(quiet, 1.0f);
    kick_frame(kick, 1.0f, 50.0f);

    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    // First kick fires.
    TEST_ASSERT_TRUE(det.process(kick, 500));
    // 50 ms later (within 100 ms refractory) - blocked.
    TEST_ASSERT_FALSE(det.process(kick, 550));
    // 80 ms after first - still blocked.
    TEST_ASSERT_FALSE(det.process(kick, 580));
    // 110 ms after first - allowed.
    TEST_ASSERT_TRUE(det.process(kick, 610));
}

// ---------------------------------------------------------------------------
// 120 BPM kick train (one kick every 500 ms) fires at the expected rate.
// ---------------------------------------------------------------------------

static void test_120bpm_kick_train_fires_at_expected_rate(void) {
    BeatDetector det;
    SpectrumFrame quiet, kick;
    quiet_noise_frame(quiet, 1.0f);
    kick_frame(kick, 1.0f, 50.0f);

    // Simulate FFT cadence at 40 Hz (one frame every 25 ms). At 120
    // BPM kicks fire every 500 ms = every 20 frames. Run 8 seconds.
    const uint32_t frame_dt_ms      = 25;
    const uint32_t kick_period_frms = 20;
    const size_t   total_frames     = 320;

    int fires = 0;
    for (size_t i = 0; i < total_frames; ++i) {
        const SpectrumFrame& f = ((i % kick_period_frms) == 0 && i >= 20) ? kick : quiet;
        if (det.process(f, i * frame_dt_ms)) ++fires;
    }
    // 8 seconds of 120 BPM = 16 beats. Allow ±1 for warm-up edge.
    TEST_ASSERT_INT_WITHIN(1, 15, fires);
}

// ---------------------------------------------------------------------------
// Unwatched bands (treble) don't trigger a kick beat.
// ---------------------------------------------------------------------------

static void test_treble_spike_does_not_fire_kick_beat(void) {
    BeatDetector det;
    SpectrumFrame quiet, treble;
    quiet_noise_frame(quiet, 1.0f);
    treble_spike_frame(treble, 1.0f, 50.0f);

    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    int fires = 0;
    for (uint32_t t = 20; t < 50; ++t) {
        if (det.process(treble, t * 25)) ++fires;
    }
    // Treble spikes are above the watched bass region; should not fire.
    TEST_ASSERT_EQUAL_INT(0, fires);
}

// ---------------------------------------------------------------------------
// Tunable threshold_k: high k makes detection less sensitive.
// ---------------------------------------------------------------------------

static void test_higher_threshold_suppresses_marginal_kicks(void) {
    // Real audio always has frame-to-frame jitter in the noise floor.
    // Use a varying-noise stream so std_dev is non-zero and threshold_k
    // actually does something. With uniform noise, std_dev = 0 and the
    // threshold collapses to just the mean regardless of k - that's a
    // real-world degenerate case we don't need to test here.
    auto noisy_frame_at = [](size_t frame_idx) {
        SpectrumFrame f;
        // Pseudo-random per-frame, per-band offset around 5.0 of ±2.0.
        // Cheap LCG for determinism.
        uint32_t state = 0x1337u + static_cast<uint32_t>(frame_idx);
        for (size_t b = 0; b < kSpectrumBands; ++b) {
            state = state * 1103515245u + 12345u;
            const float jitter = (static_cast<float>(state >> 16) / 65535.0f) * 4.0f - 2.0f;
            f.magnitudes[b] = 5.0f + jitter;
        }
        return f;
    };
    SpectrumFrame weak_kick;
    kick_frame(weak_kick, 5.0f, 12.0f);  // ~2x noise floor in the watched bands

    // Default k=1.5 should pick up the weak kick.
    BeatDetector loose;
    for (uint32_t t = 0; t < 20; ++t) loose.process(noisy_frame_at(t), t * 25);
    const bool loose_fired = loose.process(weak_kick, 20 * 25);
    TEST_ASSERT_TRUE(loose_fired);

    // k=10 should not - weak kick is well below 10*std_dev above mean.
    BeatDetector::Config strict_cfg;
    strict_cfg.threshold_k = 10.0f;
    BeatDetector strict(strict_cfg);
    for (uint32_t t = 0; t < 20; ++t) strict.process(noisy_frame_at(t), t * 25);
    const bool strict_fired = strict.process(weak_kick, 20 * 25);
    TEST_ASSERT_FALSE(strict_fired);
}

// ---------------------------------------------------------------------------
// reset() clears history and refractory state.
// ---------------------------------------------------------------------------

static void test_reset_clears_state(void) {
    BeatDetector det;
    SpectrumFrame quiet, kick;
    quiet_noise_frame(quiet, 1.0f);
    kick_frame(kick, 1.0f, 50.0f);

    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);
    TEST_ASSERT_TRUE(det.process(kick, 500));

    det.reset();

    // Post-reset, the detector is back in warm-up; the same kick on
    // frame 0 should not fire.
    TEST_ASSERT_EQUAL_UINT(0, det.frames_seen());
    TEST_ASSERT_EQUAL_UINT(0, det.last_beat_ms());
    TEST_ASSERT_FALSE(det.process(kick, 1000));
}

// ---------------------------------------------------------------------------
// Adaptive: a louder background still admits a kick that's prominent
// against it. Documents the self-calibration property that's the
// algorithm's reason to exist.
// ---------------------------------------------------------------------------

static void test_adapts_to_louder_background(void) {
    SpectrumFrame quiet_bg, kick_against_bg;

    // Bands 0-10 at floor 30 (much higher than the kHz-region bands).
    // Models a song with sustained heavy bass.
    for (size_t i = 0; i < kSpectrumBands; ++i) quiet_bg.magnitudes[i] = 1.0f;
    for (size_t i = 0; i < 11; ++i)             quiet_bg.magnitudes[i] = 30.0f;

    // Kick is ~3x the bass floor.
    for (size_t i = 0; i < kSpectrumBands; ++i) kick_against_bg.magnitudes[i] = 1.0f;
    for (size_t i = 0; i < 11; ++i)             kick_against_bg.magnitudes[i] = 90.0f;

    BeatDetector det;
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet_bg, t * 25);
    TEST_ASSERT_TRUE(det.process(kick_against_bg, 500));
}

// ---------------------------------------------------------------------------
// Epic 4.7 Block 3: watch_start enables snare / hi-hat detectors via
// the same class instantiated with different ranges.
// ---------------------------------------------------------------------------

static void test_snare_config_fires_on_mid_band_not_kick(void) {
    // Snare config from LocalDriver: bands 11-23 (~200-2000 Hz).
    BeatDetector::Config cfg;
    cfg.watch_start  = 11;
    cfg.watch_count  = 13;
    cfg.threshold_k  = 2.0f;
    cfg.refractory_ms = 150;
    BeatDetector det(cfg);

    SpectrumFrame quiet;
    quiet_noise_frame(quiet, 1.0f);
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    // A kick-band spike (bands 0-10) must NOT fire the snare detector.
    SpectrumFrame kick;
    kick_frame(kick, 1.0f, 50.0f);
    TEST_ASSERT_FALSE(det.process(kick, 21 * 25));

    // A snare-band spike (bands 11-15) MUST fire the snare detector.
    SpectrumFrame snare;
    for (size_t i = 0; i < kSpectrumBands; ++i) snare.magnitudes[i] = 1.0f;
    for (size_t i = 11; i < 16; ++i) snare.magnitudes[i] = 50.0f;
    TEST_ASSERT_TRUE(det.process(snare, 22 * 25));
}

static void test_hihat_config_fires_on_high_band_only(void) {
    // Hi-hat config from LocalDriver: bands 27-31 (~4-8 kHz).
    BeatDetector::Config cfg;
    cfg.watch_start  = 27;
    cfg.watch_count  = 5;
    cfg.threshold_k  = 1.8f;
    cfg.refractory_ms = 80;
    BeatDetector det(cfg);

    SpectrumFrame quiet;
    quiet_noise_frame(quiet, 1.0f);
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    // Mid-band spike (bands 11-15) must NOT fire the hi-hat detector.
    SpectrumFrame snare;
    for (size_t i = 0; i < kSpectrumBands; ++i) snare.magnitudes[i] = 1.0f;
    for (size_t i = 11; i < 16; ++i) snare.magnitudes[i] = 50.0f;
    TEST_ASSERT_FALSE(det.process(snare, 21 * 25));

    // High-band spike (bands 28-30) MUST fire the hi-hat detector.
    SpectrumFrame hihat;
    for (size_t i = 0; i < kSpectrumBands; ++i) hihat.magnitudes[i] = 1.0f;
    for (size_t i = 28; i < 31; ++i) hihat.magnitudes[i] = 50.0f;
    TEST_ASSERT_TRUE(det.process(hihat, 22 * 25));
}

static void test_last_strength_zero_before_any_beat(void) {
    BeatDetector det;
    TEST_ASSERT_EQUAL_UINT8(0, det.last_strength());

    // Run quiet frames - no beats fire, strength stays 0.
    SpectrumFrame quiet;
    quiet_noise_frame(quiet, 1.0f);
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);
    TEST_ASSERT_EQUAL_UINT8(0, det.last_strength());
}

static void test_last_strength_scales_with_magnitude(void) {
    // After warm-up on quiet noise, a kick that's ~2x the threshold
    // should give strength near 128; a much larger kick should saturate
    // toward 255. The exact numbers depend on the threshold value the
    // detector computes from the history, but ordering is robust.
    BeatDetector det;
    SpectrumFrame quiet;
    quiet_noise_frame(quiet, 5.0f);
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    // Soft kick must produce a strength below the 255 ceiling so the
    // "loud > soft" comparison is meaningful (a magnitude many times
    // the threshold saturates).
    SpectrumFrame soft_kick, loud_kick;
    kick_frame(soft_kick, 5.0f,  12.0f);
    kick_frame(loud_kick, 5.0f, 200.0f);

    TEST_ASSERT_TRUE(det.process(soft_kick, 600));
    const uint8_t soft = det.last_strength();
    TEST_ASSERT_GREATER_THAN_UINT8(0, soft);

    // Reset so the loud kick doesn't fall in soft_kick's refractory
    // and the history is identical for both.
    det.reset();
    for (uint32_t t = 0; t < 20; ++t) det.process(quiet, t * 25);

    TEST_ASSERT_TRUE(det.process(loud_kick, 600));
    const uint8_t loud = det.last_strength();
    TEST_ASSERT_GREATER_THAN_UINT8(soft, loud);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    RUN_TEST(test_does_not_fire_during_warmup);
    RUN_TEST(test_warmup_does_not_pollute_baseline);
    RUN_TEST(test_steady_noise_does_not_fire);
    RUN_TEST(test_single_kick_fires_once);
    RUN_TEST(test_refractory_blocks_consecutive_kicks);
    RUN_TEST(test_120bpm_kick_train_fires_at_expected_rate);
    RUN_TEST(test_treble_spike_does_not_fire_kick_beat);
    RUN_TEST(test_higher_threshold_suppresses_marginal_kicks);
    RUN_TEST(test_reset_clears_state);
    RUN_TEST(test_adapts_to_louder_background);
    RUN_TEST(test_snare_config_fires_on_mid_band_not_kick);
    RUN_TEST(test_hihat_config_fires_on_high_band_only);
    RUN_TEST(test_last_strength_zero_before_any_beat);
    RUN_TEST(test_last_strength_scales_with_magnitude);

    return UNITY_END();
}
