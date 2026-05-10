// Minimal WAV file I/O for native test harnesses.
//
// Scope: PCM 16-bit mono only. Multi-channel, float, 24/32-bit, ADPCM,
// MP3-in-WAV are all rejected. That's the format the project's mics
// emit (16-bit mono I2S) and the format Jason should record real
// reference samples in via Audacity / DAW. Anything else is a sign
// the pipeline diverged.
//
// Why ship a writer too: the loader's roundtrip test uses it, and it
// gives Jason a way to convert captured buffers to disk without
// pulling in libsndfile or similar. Two short header dumps + std::ofstream.

#pragma once

#include "synth_samples.h"

#include <string>

namespace nocturnation {
namespace test_audio {

// Load a 16-bit PCM mono WAV file. Returns Sample with samples + rate.
// On any failure (missing file, non-PCM, multi-channel, wrong bit depth)
// returns a sample with sample_rate_hz == 0 and an empty buffer.
Sample load_wav(const std::string& path);

// Write a Sample as a 16-bit PCM mono WAV. Returns true on success.
// Sample's sample_rate_hz must be > 0.
bool write_wav(const std::string& path, const Sample& s);

// True if the sample is non-empty and has a valid sample rate. Use
// this rather than checking fields directly so the contract stays in
// one place.
inline bool is_valid(const Sample& s) {
    return s.sample_rate_hz > 0 && !s.samples.empty();
}

}  // namespace test_audio
}  // namespace nocturnation
