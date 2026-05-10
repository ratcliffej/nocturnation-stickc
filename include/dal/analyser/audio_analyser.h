// Pure audio analyser core. Vendor-neutral: takes FFT magnitudes plus
// the operating point and produces typed analyser outputs. No HAL,
// Arduino, or hardware dependency - the same code links into the
// Plus2 firmware, the S3 firmware, and native test builds.
//
// HAL backends (mic_stickcplus2.cpp / mic_stickcs3.cpp) provide the raw
// FFT magnitudes; this core composes them into the band summaries and
// spectrum frame that DAL events expose to orchestration.
//
// Hz-first design: band boundaries are specified in Hz, not bin
// numbers. Bin assignments are computed at runtime from the operating
// point; the same code path works at 16 kHz / 512 FFT (canonical
// default) and at any future operating point a host declares.
//
// Why pure functions: testable without firmware deps, no global state
// to reset between frames, deterministic for a given input. The HAL
// backends own the FFT computation and the timing; this core owns the
// mapping from raw spectrum to typed events.

#pragma once

#include "dal/analyser/band_layout.h"

#include <cstdint>
#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

// 3-band B/M/T summary. Restandardised in Epic 4.5 to evidence-based
// boundaries (see band_layout.h). Treble's upper bound is the host's
// current Nyquist - operating-point-dependent.
struct BandSummary3 {
    float bass;     // [0, 250) Hz
    float mid;      // [250, 2000) Hz
    float treble;   // [2000, Nyquist) Hz
};

// 8-band perceptual summary per Audible Genius music-production
// reference. Always populated; the Mud band (0-20 Hz) reads near-zero
// in practice on most input, and Air (6-20 kHz) is operating-point-
// dependent (truncated at Nyquist when sample rate < 40 kHz).
struct BandSummary8 {
    float mud;        // [0, 20) Hz
    float sub_bass;   // [20, 60) Hz
    float bass;       // [60, 250) Hz
    float low_mids;   // [250, 500) Hz
    float midrange;   // [500, 2000) Hz
    float high_mids;  // [2000, 4000) Hz
    float presence;   // [4000, 6000) Hz
    float air;        // [6000, 20000) Hz, truncated at Nyquist
};

// 32 log-spaced sub-band magnitudes covering [kSpectrumLoHz, Nyquist).
// Used by the adaptive-threshold beat detector (Block 3) and by future
// renderers / modulators (Epics 4.6 / 4.7).
struct SpectrumFrame {
    float magnitudes[kSpectrumBands];
};

// Compute the 3-band B/M/T summary AND the 8-band perceptual summary
// from raw FFT magnitudes. Both surfaces are filled in a single pass
// over the magnitudes; the 3-band is a strict aggregation of the
// 8-band so the two are internally consistent (no drift possible).
//
// Inputs:
//   magnitudes        - one-sided FFT output, length n_bins
//   n_bins            - number of magnitude bins (typically fft_size / 2)
//   sample_rate_hz    - sample rate the FFT was computed at
//
// Output:
//   out_8band         - per-perceptual-band energy
//   out_3band         - rolled-up B/M/T (bass = mud + sub_bass + bass; etc.)
//
// The function does not zero the output structs; callers may choose to
// memset for safety. All accumulators start at 0.0f and only populated
// bins contribute.
void compute_band_summaries(const float*  magnitudes,
                            size_t        n_bins,
                            uint32_t      sample_rate_hz,
                            BandSummary8& out_8band,
                            BandSummary3& out_3band);

// Compute the 32-band log-spaced spectrum frame from raw FFT
// magnitudes. Each output band is the sum of FFT magnitudes whose bin-
// centre frequency falls within the band's [f_lo, f_hi) range.
//
// At the canonical 16 kHz / 512 FFT operating point with 256 FFT bins
// covering 0-8 kHz, low-frequency spectrum bands may map to a single
// FFT bin or even zero bins (sub-30 Hz content); higher bands cover
// many bins each. This is the expected behaviour of log-spaced
// bucketing on linear-spaced FFT input.
//
// Bins below kSpectrumLoHz are excluded entirely; bins at or above
// Nyquist do not exist (the input is one-sided, length n_bins).
void compute_spectrum_frame(const float*    magnitudes,
                            size_t          n_bins,
                            uint32_t        sample_rate_hz,
                            SpectrumFrame&  out);

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
