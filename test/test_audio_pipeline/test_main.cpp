// Native unit tests for the audio test harness (Block 1 of Epic 4.5).
//
// What this Block delivers:
//   - Synthetic sample generators (sine / kick-train / noise / silence)
//     so the analyser tests have deterministic, reproducible signals
//     they can assert against without depending on captured audio.
//   - A 16-bit PCM mono WAV loader + writer so Jason's captured
//     reference recordings (Vengaboys, podcast, ambient room, dance
//     track with drop) can drop into test/audio_samples/ and feed the
//     same harness as the synth signals.
//
// What this Block deliberately does NOT do:
//   - Run audio through the analyser and assert beat counts / drop
//     events / band-summary energies. Those assertions land in
//     Block 2 once the analyser capability surface is in place. The
//     synth + WAV plumbing tested here is the substrate Block 2's
//     analyser-output tests will build on.

#include "synth_samples.h"
#include "wav_io.h"

#include <unity.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace nocturnation::test_audio;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Sine wave generator
// ---------------------------------------------------------------------------

static void test_sine_sample_count_matches_duration(void) {
    auto s = generate_sine(1000.0f, 100, 16000);
    TEST_ASSERT_EQUAL_UINT(16000u, s.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT(1600u, s.samples.size());
}

static void test_sine_amplitude_clamped_to_request(void) {
    auto s = generate_sine(1000.0f, 100, 16000, 0.5f);
    int16_t peak = 0;
    for (auto v : s.samples) {
        const int16_t abs_v = (v < 0) ? -v : v;
        if (abs_v > peak) peak = abs_v;
    }
    // 0.5 amplitude -> roughly 16383 peak. Allow a small margin for
    // sine quantisation; we should land within ~1% of the requested.
    TEST_ASSERT_INT_WITHIN(200, 16383, peak);
}

static void test_sine_zero_crossings_match_frequency(void) {
    // 1 kHz over 100 ms = 100 cycles = 200 zero crossings (one per
    // half-cycle, give or take a boundary).
    auto s = generate_sine(1000.0f, 100, 16000, 0.5f);
    int crossings = 0;
    for (size_t i = 1; i < s.samples.size(); ++i) {
        if ((s.samples[i - 1] < 0) != (s.samples[i] < 0)) ++crossings;
    }
    TEST_ASSERT_INT_WITHIN(2, 200, crossings);
}

static void test_sine_dc_offset_is_negligible(void) {
    // Whole-period integration of a sine over many cycles must average
    // to ~0. Tests we're producing a real sine and not e.g. a half-wave
    // rectified version.
    auto s = generate_sine(1000.0f, 1000, 16000, 0.5f);
    int64_t sum = 0;
    for (auto v : s.samples) sum += v;
    const int64_t mean_x1000 = (sum * 1000) / static_cast<int64_t>(s.samples.size());
    TEST_ASSERT_INT_WITHIN(50, 0, static_cast<int>(mean_x1000));
}

// ---------------------------------------------------------------------------
// Kick-train generator
// ---------------------------------------------------------------------------

static void test_kick_train_120bpm_produces_expected_kicks(void) {
    // 120 BPM over 4 seconds = 8 kicks at sample offsets
    // {0, 8000, 16000, 24000, 32000, 40000, 48000, 56000} - one
    // every 500 ms. Verify by checking that those exact positions
    // contain energy and that points far from any kick are silent.
    auto s = generate_kick_train(120.0f, 4000, 16000, 0.7f);
    TEST_ASSERT_EQUAL_UINT(64000u, s.samples.size());

    // Kick "near peak" is within the first ~80 samples of each onset
    // (envelope tau = 240 samples; first cycle of 80 Hz carrier
    // peaks around sample 50). Look for any sample exceeding half
    // peak amplitude in the first 100 samples of each expected kick.
    const uint32_t expected_starts[] = {0, 8000, 16000, 24000,
                                        32000, 40000, 48000, 56000};
    for (uint32_t start : expected_starts) {
        int16_t peak = 0;
        for (uint32_t i = start; i < start + 100; ++i) {
            const int16_t abs_v = (s.samples[i] < 0) ? -s.samples[i] : s.samples[i];
            if (abs_v > peak) peak = abs_v;
        }
        // Half of peak amplitude (0.7 * 32767 / 2 ≈ 11470).
        TEST_ASSERT_GREATER_THAN_INT(10000, peak);
    }

    // And verify gaps are quiet: midpoint between kicks (offset 4000,
    // 12000, ...) should be effectively silent.
    for (uint32_t mid = 4000; mid < s.samples.size(); mid += 8000) {
        int16_t peak = 0;
        for (uint32_t i = mid - 50; i < mid + 50; ++i) {
            const int16_t abs_v = (s.samples[i] < 0) ? -s.samples[i] : s.samples[i];
            if (abs_v > peak) peak = abs_v;
        }
        TEST_ASSERT_LESS_THAN_INT(100, peak);
    }
}

static void test_kick_train_silence_between_kicks(void) {
    // Sample halfway between kicks at 120 BPM = 250 ms in. The kick
    // envelope decay (60 ms duration, 4-tau) means by 60ms the signal
    // has dropped to e^-4 ~= 1.8% of peak. By 250 ms we should be at
    // dead silence.
    auto s = generate_kick_train(120.0f, 4000, 16000, 0.7f);
    const size_t mid_kick = 16000u / 4u;  // 250 ms = 4000 samples
    int16_t max_in_gap = 0;
    for (size_t i = mid_kick - 100; i < mid_kick + 100; ++i) {
        const int16_t abs_v = (s.samples[i] < 0) ? -s.samples[i] : s.samples[i];
        if (abs_v > max_in_gap) max_in_gap = abs_v;
    }
    TEST_ASSERT_LESS_THAN_INT(100, max_in_gap);
}

static void test_kick_train_zero_bpm_returns_silence(void) {
    // Edge case: BPM <= 0 should return a silent buffer rather than
    // divide-by-zero or loop forever. Documents the contract.
    auto s = generate_kick_train(0.0f, 100, 16000, 0.7f);
    TEST_ASSERT_EQUAL_UINT(1600u, s.samples.size());
    for (auto v : s.samples) TEST_ASSERT_EQUAL_INT16(0, v);
}

// ---------------------------------------------------------------------------
// White noise generator
// ---------------------------------------------------------------------------

static void test_white_noise_amplitude_bounded(void) {
    auto s = generate_white_noise(1000, 16000, 0.3f, 42);
    int16_t peak = 0;
    for (auto v : s.samples) {
        const int16_t abs_v = (v < 0) ? -v : v;
        if (abs_v > peak) peak = abs_v;
    }
    // 0.3 amplitude -> 9830 peak. Uniform distribution should reach
    // close to but not exceed this within a 1-sec window.
    TEST_ASSERT_LESS_OR_EQUAL_INT(9830 + 50, peak);
    TEST_ASSERT_GREATER_OR_EQUAL_INT(8000, peak);  // sanity: non-trivial signal
}

static void test_white_noise_deterministic_for_same_seed(void) {
    auto a = generate_white_noise(100, 16000, 0.5f, 42);
    auto b = generate_white_noise(100, 16000, 0.5f, 42);
    TEST_ASSERT_EQUAL_UINT(a.samples.size(), b.samples.size());
    for (size_t i = 0; i < a.samples.size(); ++i) {
        TEST_ASSERT_EQUAL_INT16(a.samples[i], b.samples[i]);
    }
}

static void test_white_noise_differs_for_different_seeds(void) {
    auto a = generate_white_noise(100, 16000, 0.5f, 1);
    auto b = generate_white_noise(100, 16000, 0.5f, 2);
    int matching = 0;
    for (size_t i = 0; i < a.samples.size(); ++i) {
        if (a.samples[i] == b.samples[i]) ++matching;
    }
    // Two LCG streams with different seeds should disagree on nearly
    // every sample. Anything more than a few percent matches means the
    // seeding isn't taking effect.
    TEST_ASSERT_LESS_THAN_INT(static_cast<int>(a.samples.size() / 20), matching);
}

// ---------------------------------------------------------------------------
// Silence generator
// ---------------------------------------------------------------------------

static void test_silence_is_all_zero(void) {
    auto s = generate_silence(100, 16000);
    TEST_ASSERT_EQUAL_UINT(1600u, s.samples.size());
    for (auto v : s.samples) TEST_ASSERT_EQUAL_INT16(0, v);
}

// ---------------------------------------------------------------------------
// WAV I/O roundtrip
// ---------------------------------------------------------------------------

static std::string tmp_wav_path(const char* tag) {
    // Use $TMPDIR if set (CI environments), otherwise /tmp. PID in the
    // filename so concurrent test runs don't trample each other.
    const char* dir = std::getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    std::string p = dir;
    if (p.back() != '/') p += '/';
    p += "noct_wav_test_";
    p += tag;
    char pid_buf[32];
    std::snprintf(pid_buf, sizeof(pid_buf), "_%d.wav", static_cast<int>(getpid()));
    p += pid_buf;
    return p;
}

static void test_wav_roundtrip_sine(void) {
    auto orig = generate_sine(1000.0f, 100, 16000, 0.5f);
    const std::string path = tmp_wav_path("sine");
    TEST_ASSERT_TRUE(write_wav(path, orig));

    auto loaded = load_wav(path);
    TEST_ASSERT_TRUE(is_valid(loaded));
    TEST_ASSERT_EQUAL_UINT(orig.sample_rate_hz, loaded.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT(orig.samples.size(), loaded.samples.size());
    for (size_t i = 0; i < orig.samples.size(); ++i) {
        TEST_ASSERT_EQUAL_INT16(orig.samples[i], loaded.samples[i]);
    }

    std::remove(path.c_str());
}

static void test_wav_roundtrip_kick_train(void) {
    auto orig = generate_kick_train(120.0f, 1000, 16000, 0.7f);
    const std::string path = tmp_wav_path("kick");
    TEST_ASSERT_TRUE(write_wav(path, orig));

    auto loaded = load_wav(path);
    TEST_ASSERT_TRUE(is_valid(loaded));
    TEST_ASSERT_EQUAL_UINT(orig.samples.size(), loaded.samples.size());
    // Spot-check a few sample positions rather than every single one;
    // the sine roundtrip already covers byte-exact equality.
    for (size_t i : {0u, 100u, 7999u, 8000u, 15999u}) {
        TEST_ASSERT_EQUAL_INT16(orig.samples[i], loaded.samples[i]);
    }

    std::remove(path.c_str());
}

static void test_wav_load_missing_file_returns_invalid(void) {
    auto s = load_wav("/tmp/this_path_definitely_does_not_exist_noct.wav");
    TEST_ASSERT_FALSE(is_valid(s));
    TEST_ASSERT_EQUAL_UINT(0u, s.sample_rate_hz);
    TEST_ASSERT_EQUAL_UINT(0u, s.samples.size());
}

static void test_wav_write_then_load_preserves_sample_rate(void) {
    // Non-default rate to confirm sample_rate_hz round-trips correctly,
    // not just hard-coded 16000 somewhere in the writer.
    auto orig = generate_sine(440.0f, 50, 48000, 0.4f);
    const std::string path = tmp_wav_path("48k");
    TEST_ASSERT_TRUE(write_wav(path, orig));

    auto loaded = load_wav(path);
    TEST_ASSERT_TRUE(is_valid(loaded));
    TEST_ASSERT_EQUAL_UINT(48000u, loaded.sample_rate_hz);

    std::remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();

    // Sine
    RUN_TEST(test_sine_sample_count_matches_duration);
    RUN_TEST(test_sine_amplitude_clamped_to_request);
    RUN_TEST(test_sine_zero_crossings_match_frequency);
    RUN_TEST(test_sine_dc_offset_is_negligible);

    // Kick train
    RUN_TEST(test_kick_train_120bpm_produces_expected_kicks);
    RUN_TEST(test_kick_train_silence_between_kicks);
    RUN_TEST(test_kick_train_zero_bpm_returns_silence);

    // White noise
    RUN_TEST(test_white_noise_amplitude_bounded);
    RUN_TEST(test_white_noise_deterministic_for_same_seed);
    RUN_TEST(test_white_noise_differs_for_different_seeds);

    // Silence
    RUN_TEST(test_silence_is_all_zero);

    // WAV I/O
    RUN_TEST(test_wav_roundtrip_sine);
    RUN_TEST(test_wav_roundtrip_kick_train);
    RUN_TEST(test_wav_load_missing_file_returns_invalid);
    RUN_TEST(test_wav_write_then_load_preserves_sample_rate);

    return UNITY_END();
}
