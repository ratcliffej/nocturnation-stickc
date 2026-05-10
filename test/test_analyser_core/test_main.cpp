// Native unit tests for the pure DAL audio analyser core (Block 2a of
// Epic 4.5).
//
// Approach: feed in synthetic magnitude vectors (e.g. spike at FFT bin
// K) rather than running a real FFT in the test. This isolates the
// band-routing logic from FFT correctness; FFT correctness is the
// HAL backend's responsibility (and is verified empirically on
// hardware - tests at this layer would just re-derive what FFT
// libraries already guarantee).
//
// Each test sets up a magnitudes[N/2] buffer, places known energy at
// known bins, calls compute_band_summaries() / compute_spectrum_frame(),
// and asserts the output bands receive energy in the expected places.

#include "dal/analyser/audio_analyser.h"

#include <unity.h>

#include <cmath>
#include <cstring>

using namespace nocturnation::dal::analyser;

namespace {

// Canonical operating point used by most tests.
constexpr uint32_t kSampleRateHz = 16000;
constexpr size_t   kFftSize      = 512;
constexpr size_t   kNBins        = kFftSize / 2;  // one-sided FFT output

// Bin -> Hz at canonical operating point: bin i = i * 16000 / 512 = i * 31.25.
//   Bin 0   = 0     Hz   (DC)
//   Bin 1   = 31.25 Hz   (Mud / Sub Bass boundary - exact placement depends)
//   Bin 4   = 125   Hz   (Bass)
//   Bin 32  = 1000  Hz   (Midrange)
//   Bin 96  = 3000  Hz   (High Mids)
//   Bin 160 = 5000  Hz   (Presence)
//   Bin 224 = 7000  Hz   (Air)
constexpr float kBinHz = 16000.0f / 512.0f;  // 31.25

// Helper: zero-fill a magnitudes buffer then set a single bin.
void set_single_bin(float* mags, size_t n, size_t bin, float value) {
    for (size_t i = 0; i < n; ++i) mags[i] = 0.0f;
    if (bin < n) mags[bin] = value;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Per-band routing: a single-bin spike lands in the expected perceptual band.
// ---------------------------------------------------------------------------

static void test_single_bin_at_100hz_lands_in_bass(void) {
    // 100 Hz at 31.25 Hz/bin -> bin 3.2; round to bin 3 (93.75 Hz).
    // 93.75 Hz is in [60, 250) - the Bass band.
    float mags[kNBins];
    set_single_bin(mags, kNBins, 3, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.mud);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.sub_bass);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.low_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.midrange);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.high_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.presence);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.air);
}

static void test_single_bin_at_40hz_lands_in_sub_bass(void) {
    // 40 Hz / 31.25 = bin 1.28 -> bin 1 (31.25 Hz). 31.25 Hz is in
    // [20, 60) - the Sub Bass band.
    float mags[kNBins];
    set_single_bin(mags, kNBins, 1, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.sub_bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.bass);
}

static void test_single_bin_at_350hz_lands_in_low_mids(void) {
    // 350 / 31.25 = 11.2 -> bin 11 (343.75 Hz). In [250, 500).
    float mags[kNBins];
    set_single_bin(mags, kNBins, 11, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.low_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.midrange);
}

static void test_single_bin_at_1khz_lands_in_midrange(void) {
    // 1000 / 31.25 = 32 exact. Bin 32 = 1000 Hz, in [500, 2000).
    float mags[kNBins];
    set_single_bin(mags, kNBins, 32, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.midrange);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.low_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.high_mids);
}

static void test_single_bin_at_3khz_lands_in_high_mids(void) {
    // 3000 / 31.25 = 96 exact. Bin 96 = 3000 Hz, in [2000, 4000).
    float mags[kNBins];
    set_single_bin(mags, kNBins, 96, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.high_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.midrange);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.presence);
}

static void test_single_bin_at_5khz_lands_in_presence(void) {
    // 5000 / 31.25 = 160. Bin 160 = 5000 Hz, in [4000, 6000).
    float mags[kNBins];
    set_single_bin(mags, kNBins, 160, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.presence);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.high_mids);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.air);
}

static void test_single_bin_at_7khz_lands_in_air(void) {
    // 7000 / 31.25 = 224. Bin 224 = 7000 Hz, in [6000, 20000).
    float mags[kNBins];
    set_single_bin(mags, kNBins, 224, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.air);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.presence);
}

static void test_dc_bin_lands_in_mud(void) {
    // Bin 0 (DC, 0 Hz) is in [0, 20) - the Mud band. Documents the
    // contract that DC is captured even though it's mostly artefact:
    // future consumers (sub-sonic content audit, low-frequency spike
    // detection) expect to find it in mud.
    float mags[kNBins];
    set_single_bin(mags, kNBins, 0, 1.0f);

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.mud);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.sub_bass);
}

// ---------------------------------------------------------------------------
// 3-band roll-up: strict aggregation of 8-band, no drift.
// ---------------------------------------------------------------------------

static void test_3band_is_strict_rollup_of_8band(void) {
    // Spread energy across all 8 perceptual bands and verify the
    // 3-band sums are exact aggregations.
    float mags[kNBins];
    for (size_t i = 0; i < kNBins; ++i) mags[i] = 0.0f;
    mags[0]   = 0.10f;   // Mud
    mags[1]   = 0.20f;   // Sub Bass
    mags[3]   = 0.30f;   // Bass
    mags[11]  = 0.40f;   // Low Mids
    mags[32]  = 0.50f;   // Midrange
    mags[96]  = 0.60f;   // High Mids
    mags[160] = 0.70f;   // Presence
    mags[224] = 0.80f;   // Air

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(b8.mud + b8.sub_bass + b8.bass,         b3.bass);
    TEST_ASSERT_EQUAL_FLOAT(b8.low_mids + b8.midrange,              b3.mid);
    TEST_ASSERT_EQUAL_FLOAT(b8.high_mids + b8.presence + b8.air,    b3.treble);
}

// ---------------------------------------------------------------------------
// Hz-first design: same Hz boundaries work at a different operating point.
// ---------------------------------------------------------------------------

static void test_band_layout_works_at_48khz_2048fft(void) {
    // Future operating point: 48 kHz / 2048 FFT, n_bins = 1024.
    // Bin spacing = 48000 / 2048 = 23.4375 Hz.
    // 1 kHz / 23.4375 = 42.66 -> bin 42 (= 984.4 Hz, in midrange).
    // 5 kHz / 23.4375 = 213.3 -> bin 213 (= 4992.2 Hz, in presence).
    // Air ceiling at 20 kHz: 20000 / 23.4375 = 853.3 -> bin 853.
    // Bins above that should be dropped (perceptual ceiling truncates).
    constexpr uint32_t sr   = 48000;
    constexpr size_t   nb   = 1024;
    float mags[nb];
    for (size_t i = 0; i < nb; ++i) mags[i] = 0.0f;
    mags[42]  = 1.0f;   // ~1 kHz
    mags[213] = 1.0f;   // ~5 kHz
    mags[900] = 1.0f;   // ~21 kHz - above Air ceiling, should be dropped

    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, nb, sr, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.midrange);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, b8.presence);
    // bin 900 is at ~21 kHz - above the 20 kHz Air ceiling, dropped.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.air);
}

// ---------------------------------------------------------------------------
// Spectrum frame: 32 log-spaced bands cover [30 Hz, Nyquist).
// ---------------------------------------------------------------------------

static void test_spectrum_frame_initialised_to_zero(void) {
    float mags[kNBins];
    for (size_t i = 0; i < kNBins; ++i) mags[i] = 0.0f;

    SpectrumFrame sf;
    // Pre-fill with non-zero to verify the function clears the output.
    for (size_t i = 0; i < kSpectrumBands; ++i) sf.magnitudes[i] = 99.0f;

    compute_spectrum_frame(mags, kNBins, kSampleRateHz, sf);

    for (size_t i = 0; i < kSpectrumBands; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(0.0f, sf.magnitudes[i]);
    }
}

static void test_spectrum_frame_routes_to_correct_band(void) {
    // 1 kHz / 16 kHz operating point. kSpectrumLoHz = 30 Hz; Nyquist =
    // 8000 Hz. Ratio per band = (8000/30)^(1/32) ≈ 1.1907.
    // Band index for 1 kHz: floor(log(1000/30) / log(1.1907)) = 20.
    // Asserted by recomputing rather than hard-coding the index, so
    // the test remains correct if kSpectrumBands or kSpectrumLoHz
    // are tuned in future.
    float mags[kNBins];
    set_single_bin(mags, kNBins, 32, 1.0f);  // 1 kHz at bin 32

    SpectrumFrame sf;
    compute_spectrum_frame(mags, kNBins, kSampleRateHz, sf);

    // Find the unique band that received the spike.
    int hit_band = -1;
    for (size_t i = 0; i < kSpectrumBands; ++i) {
        if (sf.magnitudes[i] != 0.0f) {
            TEST_ASSERT_EQUAL_INT(-1, hit_band);  // exactly one band
            hit_band = static_cast<int>(i);
            TEST_ASSERT_EQUAL_FLOAT(1.0f, sf.magnitudes[i]);
        }
    }
    TEST_ASSERT_NOT_EQUAL_INT(-1, hit_band);

    // Confirm 1 kHz falls within hit_band's [f_lo, f_hi).
    const float ratio   = std::pow(8000.0f / 30.0f,
                                   1.0f / static_cast<float>(kSpectrumBands));
    const float f_lo    = 30.0f * std::pow(ratio, static_cast<float>(hit_band));
    const float f_hi    = 30.0f * std::pow(ratio, static_cast<float>(hit_band + 1));
    TEST_ASSERT_TRUE(f_lo <= 1000.0f && 1000.0f < f_hi);
}

static void test_spectrum_frame_excludes_subsonic_bins(void) {
    // Bin 0 (DC) and bin 1 (~31 Hz) - both at or near the kSpectrumLoHz
    // floor. Bin 0 is below 30 Hz and should be excluded entirely;
    // bin 1 (31.25 Hz) is just above and should land in band 0.
    float mags[kNBins];
    for (size_t i = 0; i < kNBins; ++i) mags[i] = 0.0f;
    mags[0] = 5.0f;   // DC - should be excluded
    mags[1] = 1.0f;   // 31.25 Hz - should land in spectrum band 0

    SpectrumFrame sf;
    compute_spectrum_frame(mags, kNBins, kSampleRateHz, sf);

    TEST_ASSERT_EQUAL_FLOAT(1.0f, sf.magnitudes[0]);
    // DC didn't add 5.0 to band 0; it was excluded.
    TEST_ASSERT_EQUAL_FLOAT(0.0f, sf.magnitudes[31]);
}

static void test_spectrum_frame_total_energy_preserved_for_in_range_bins(void) {
    // Sum of spectrum frame == sum of in-range FFT magnitudes (sub-30Hz
    // bins excluded). Verifies no double-counting and no missed bins
    // in the log-spaced bucketing.
    float mags[kNBins];
    for (size_t i = 0; i < kNBins; ++i) mags[i] = 1.0f;  // unit per bin

    SpectrumFrame sf;
    compute_spectrum_frame(mags, kNBins, kSampleRateHz, sf);

    // Bins above kSpectrumLoHz (30 Hz): bin >= ceil(30 / 31.25) = 1.
    // So bins 1..255 contribute, total = 255.
    float total = 0.0f;
    for (size_t i = 0; i < kSpectrumBands; ++i) total += sf.magnitudes[i];
    TEST_ASSERT_EQUAL_FLOAT(255.0f, total);
}

// ---------------------------------------------------------------------------
// Pathological inputs: don't crash, produce zero output.
// ---------------------------------------------------------------------------

static void test_null_magnitudes_produces_zero_output(void) {
    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(nullptr, kNBins, kSampleRateHz, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.mud);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.air);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b3.bass);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b3.mid);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b3.treble);
}

static void test_zero_sample_rate_produces_zero_output(void) {
    float mags[kNBins];
    for (size_t i = 0; i < kNBins; ++i) mags[i] = 1.0f;
    BandSummary8 b8{};
    BandSummary3 b3{};
    compute_band_summaries(mags, kNBins, /*sr=*/0, b8, b3);

    TEST_ASSERT_EQUAL_FLOAT(0.0f, b8.mud);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, b3.bass);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    // Per-band routing
    RUN_TEST(test_single_bin_at_100hz_lands_in_bass);
    RUN_TEST(test_single_bin_at_40hz_lands_in_sub_bass);
    RUN_TEST(test_single_bin_at_350hz_lands_in_low_mids);
    RUN_TEST(test_single_bin_at_1khz_lands_in_midrange);
    RUN_TEST(test_single_bin_at_3khz_lands_in_high_mids);
    RUN_TEST(test_single_bin_at_5khz_lands_in_presence);
    RUN_TEST(test_single_bin_at_7khz_lands_in_air);
    RUN_TEST(test_dc_bin_lands_in_mud);

    // 3-band roll-up consistency
    RUN_TEST(test_3band_is_strict_rollup_of_8band);

    // Hz-first portability
    RUN_TEST(test_band_layout_works_at_48khz_2048fft);

    // Spectrum frame
    RUN_TEST(test_spectrum_frame_initialised_to_zero);
    RUN_TEST(test_spectrum_frame_routes_to_correct_band);
    RUN_TEST(test_spectrum_frame_excludes_subsonic_bins);
    RUN_TEST(test_spectrum_frame_total_energy_preserved_for_in_range_bins);

    // Pathological inputs
    RUN_TEST(test_null_magnitudes_produces_zero_output);
    RUN_TEST(test_zero_sample_rate_produces_zero_output);

    return UNITY_END();
}
