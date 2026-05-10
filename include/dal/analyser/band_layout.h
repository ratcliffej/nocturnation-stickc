// Hz-first band layout definitions for the audio analyser.
//
// Band boundaries are specified in Hz, not bin numbers. Bin assignments
// are computed at runtime from the current `(sample_rate_hz, fft_size)`
// operating point. The same code path produces correct mappings whether
// the analyser is running at 16 kHz / 512 FFT (canonical default) or
// 48 kHz / 2048 FFT (future operating point on capable hosts) - no
// per-operating-point conditionals in the analyser core.
//
// The 8-band perceptual split-points are taken from the Audible Genius
// music-production reference (cited in Epic 4.5). The 3-band B/M/T
// roll-up uses the same boundaries internal to the 8-band: <250 Hz at
// the Bass→Low Mids transition, 2 kHz at the Midrange→High Mids
// transition.

#pragma once

#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

// 8-band perceptual layout per Audible Genius. Boundaries are inclusive
// at the lower edge, exclusive at the upper edge: Mud is [0, 20), Sub
// Bass is [20, 60), and so on. The Air band's 20 kHz upper bound is
// the perceptual ceiling; at sample rates below 40 kHz the band is
// truncated at Nyquist (sample_rate / 2) and that's reported honestly
// rather than masked.
struct PerceptualBoundsHz {
    static constexpr float mud_hi       =    20.0f;  //  Mud         (0 - 20 Hz)
    static constexpr float sub_bass_hi  =    60.0f;  //  Sub Bass    (20 - 60 Hz)
    static constexpr float bass_hi      =   250.0f;  //  Bass        (60 - 250 Hz)
    static constexpr float low_mids_hi  =   500.0f;  //  Low Mids    (250 - 500 Hz)
    static constexpr float midrange_hi  =  2000.0f;  //  Midrange    (500 Hz - 2 kHz)
    static constexpr float high_mids_hi =  4000.0f;  //  High Mids   (2 kHz - 4 kHz)
    static constexpr float presence_hi  =  6000.0f;  //  Presence    (4 kHz - 6 kHz)
    static constexpr float air_hi       = 20000.0f;  //  Air         (6 kHz - 20 kHz)
};

// 3-band B/M/T roll-up. Same boundaries as the 8-band, just coarser:
//   bass    = [0, 250)        = Mud + Sub Bass + Bass
//   mid     = [250, 2000)     = Low Mids + Midrange
//   treble  = [2000, Nyquist) = High Mids + Presence + Air
struct BmtBoundsHz {
    static constexpr float bass_hi = PerceptualBoundsHz::bass_hi;     //  250 Hz
    static constexpr float mid_hi  = PerceptualBoundsHz::midrange_hi; // 2000 Hz
};

// Number of log-spaced bands in the spectrum-frame surface. 32 chosen
// to give roughly quarter-octave resolution across 8 octaves of the
// canonical 16 kHz / 512 FFT operating point (30 Hz to 8 kHz Nyquist =
// log2(8000/30) ≈ 8.06 octaves).
constexpr size_t kSpectrumBands = 32;

// Lower frequency bound for the spectrum-frame surface. Below 30 Hz
// is sub-audible / mostly DC artefact at the canonical operating point;
// keeping the spectrum's display range above this floor concentrates
// resolution on the audible bands. The Mud perceptual band still
// captures everything below 20 Hz for consumers that want it.
constexpr float kSpectrumLoHz = 30.0f;

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
