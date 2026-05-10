// Synthetic audio sample generators for native-test analyser work.
//
// These produce deterministic, reproducible test signals as 16-bit PCM
// mono buffers - the same shape captured-audio WAVs deliver via the
// loader in wav_io.h. Together they give the test harness one uniform
// surface (load Sample, run through analyser, assert) regardless of
// whether the source is synthetic (committed in code) or captured
// (real recording on disk).
//
// Why synth at all when we want real audio? Three reasons:
//   1. Sine waves at known frequencies are the only honest way to test
//      that the band-summary surface assigns energy to the correct band
//      (a 1 kHz sine MUST land in midrange, period). Real music's
//      content is too entangled for unambiguous per-band assertions.
//   2. Kick trains at exact BPM let beat-detection regression tests
//      assert hit count without a human listening for misses.
//   3. Synth signals are version-controlled with the test code; real
//      WAVs are not (large binary, gitignored). The synth subset means
//      the test suite passes for any contributor on a fresh checkout
//      without needing to download recordings.

#pragma once

#include <cstdint>
#include <vector>

namespace nocturnation {
namespace test_audio {

// A single mono PCM 16-bit audio buffer with its sample rate. Output of
// every generator and of the WAV loader.
struct Sample {
    std::vector<int16_t> samples;
    uint32_t sample_rate_hz = 0;
};

// Pure sine wave at freq_hz, ±(amplitude × INT16_MAX). For testing band-
// summary assignment: a 100 Hz sine should peak the bass band, 1 kHz the
// midrange band, 4 kHz the high-mids band, etc.
//
// amplitude is clamped to [0.0, 1.0]; values outside that range get
// clipped to int16 hard limits in the output.
Sample generate_sine(float    freq_hz,
                     uint32_t duration_ms,
                     uint32_t sample_rate_hz = 16000,
                     float    amplitude       = 0.5f);

// Kick-drum-like impulse train at the given BPM. Each kick is a damped
// 80 Hz sine with ~60 ms exponential decay - bass-heavy onset followed
// by silence. Provides a regular, known-tempo bass-band onset for beat-
// detection regression tests.
//
// At 120 BPM that's one kick every 500 ms. Expected beat-detection
// output: ~120 BEAT_DETECTED events per minute, ±1.
Sample generate_kick_train(float    bpm,
                           uint32_t duration_ms,
                           uint32_t sample_rate_hz = 16000,
                           float    amplitude       = 0.7f);

// Pseudo-random uniform noise normalised to ±(amplitude × INT16_MAX).
// Deterministic for a given seed so beat-detection false-positive rate
// is measurable run-to-run. Default amplitude 0.3 keeps the level low
// enough to model "ambient room noise" rather than a loud test signal.
Sample generate_white_noise(uint32_t duration_ms,
                            uint32_t sample_rate_hz = 16000,
                            float    amplitude       = 0.3f,
                            uint32_t seed            = 0x1337);

// All-zero buffer. Used to exercise volume gates and confirm the
// analyser doesn't fire phantom beats on silence.
Sample generate_silence(uint32_t duration_ms,
                        uint32_t sample_rate_hz = 16000);

}  // namespace test_audio
}  // namespace nocturnation
