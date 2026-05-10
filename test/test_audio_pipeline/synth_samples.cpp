#include "synth_samples.h"

#include <cmath>
#include <cstdint>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace nocturnation {
namespace test_audio {

namespace {

// Saturating int32 -> int16 cast.
inline int16_t sat16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return static_cast<int16_t>(v);
}

// Sample count for a given duration. Use uint64 in the multiply so
// long durations (minutes) at 48 kHz don't overflow during scaling.
inline size_t samples_for(uint32_t duration_ms, uint32_t sample_rate_hz) {
    return static_cast<size_t>(
        (static_cast<uint64_t>(duration_ms) *
         static_cast<uint64_t>(sample_rate_hz)) / 1000ull);
}

}  // namespace

Sample generate_sine(float freq_hz, uint32_t duration_ms,
                     uint32_t sample_rate_hz, float amplitude) {
    Sample s;
    s.sample_rate_hz = sample_rate_hz;
    const size_t n = samples_for(duration_ms, sample_rate_hz);
    s.samples.reserve(n);

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;

    const float omega = 2.0f * static_cast<float>(M_PI) * freq_hz /
                        static_cast<float>(sample_rate_hz);
    const float amp16 = amplitude * 32767.0f;

    for (size_t i = 0; i < n; ++i) {
        s.samples.push_back(
            sat16(static_cast<int32_t>(std::sin(omega * static_cast<float>(i)) * amp16)));
    }
    return s;
}

Sample generate_kick_train(float bpm, uint32_t duration_ms,
                           uint32_t sample_rate_hz, float amplitude) {
    Sample s;
    s.sample_rate_hz = sample_rate_hz;
    const size_t n = samples_for(duration_ms, sample_rate_hz);
    s.samples.assign(n, 0);

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;
    if (bpm <= 0.0f) return s;

    // Each kick: 80 Hz sine under a fast exponential envelope. 60 ms
    // active window covers ~5 cycles of the carrier - enough for the
    // analyser's bass band to register the burst, short enough that
    // even 240 BPM (4 Hz) doesn't overlap consecutive kicks.
    const float    kick_freq        = 80.0f;
    const uint32_t kick_dur_samples = sample_rate_hz * 60u / 1000u;
    const float    decay_tau        = static_cast<float>(kick_dur_samples) / 4.0f;
    const float    omega            = 2.0f * static_cast<float>(M_PI) * kick_freq /
                                      static_cast<float>(sample_rate_hz);
    const float    amp16            = amplitude * 32767.0f;
    const uint32_t period_samples   = static_cast<uint32_t>(
        60.0f * static_cast<float>(sample_rate_hz) / bpm);

    for (size_t kick_start = 0;
         kick_start + kick_dur_samples <= n;
         kick_start += period_samples) {
        for (uint32_t i = 0; i < kick_dur_samples; ++i) {
            const float env = std::exp(-static_cast<float>(i) / decay_tau);
            const float v   = std::sin(omega * static_cast<float>(i)) * env;
            const int32_t accum = static_cast<int32_t>(s.samples[kick_start + i]) +
                                  static_cast<int32_t>(v * amp16);
            s.samples[kick_start + i] = sat16(accum);
        }
    }
    return s;
}

Sample generate_white_noise(uint32_t duration_ms, uint32_t sample_rate_hz,
                            float amplitude, uint32_t seed) {
    Sample s;
    s.sample_rate_hz = sample_rate_hz;
    const size_t n = samples_for(duration_ms, sample_rate_hz);
    s.samples.reserve(n);

    if (amplitude < 0.0f) amplitude = 0.0f;
    if (amplitude > 1.0f) amplitude = 1.0f;

    // Linear congruential generator. Seed-deterministic; not
    // statistically high-quality, but uniform enough for noise-floor
    // and false-positive testing. Numbers are the standard glibc
    // params.
    uint32_t state = seed;
    auto next_u32 = [&]() -> uint32_t {
        state = state * 1103515245u + 12345u;
        return state;
    };

    const float amp16 = amplitude * 32767.0f;
    for (size_t i = 0; i < n; ++i) {
        // Map [0, 2^32) -> [-1.0, 1.0).
        const float u01 = static_cast<float>(next_u32()) / 4294967296.0f;
        const float v   = (u01 * 2.0f) - 1.0f;
        s.samples.push_back(sat16(static_cast<int32_t>(v * amp16)));
    }
    return s;
}

Sample generate_silence(uint32_t duration_ms, uint32_t sample_rate_hz) {
    Sample s;
    s.sample_rate_hz = sample_rate_hz;
    s.samples.assign(samples_for(duration_ms, sample_rate_hz), 0);
    return s;
}

}  // namespace test_audio
}  // namespace nocturnation
