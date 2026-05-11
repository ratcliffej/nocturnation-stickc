// MusicDescriptors implementation (Epic 4.7 Block 3).

#include "dal/analyser/music_descriptors.h"

#include <cmath>

namespace nocturnation {
namespace dal {
namespace analyser {

namespace {

inline uint8_t clamp_to_u8(float v) {
    if (v < 0.0f)    return 0;
    if (v > 255.0f)  return 255;
    return static_cast<uint8_t>(v);
}

// Compute spectral centroid as the magnitude-weighted average band
// index, normalised to 0..255. Silence (sum == 0) returns 0.
uint8_t compute_centroid_u8(const SpectrumFrame& spec) {
    float sum_mag      = 0.0f;
    float sum_idx_mag  = 0.0f;
    for (size_t i = 0; i < kSpectrumBands; ++i) {
        const float m = spec.magnitudes[i];
        sum_mag     += m;
        sum_idx_mag += static_cast<float>(i) * m;
    }
    if (sum_mag <= 0.0f) return 0;
    const float band_centroid = sum_idx_mag / sum_mag;
    // Map [0, kSpectrumBands) -> [0, 255].
    const float v = band_centroid * 255.0f
                  / static_cast<float>(kSpectrumBands - 1);
    return clamp_to_u8(v);
}

}  // namespace

void MusicDescriptors::reset() {
    for (size_t i = 0; i < kDensityRingCap; ++i) event_ts_[i] = 0;
    event_count_     = 0;
    event_head_      = 0;
    energy_smoothed_ = 0.0f;
    centroid_        = 0;
    energy_          = 0;
    density_         = 0;
}

void MusicDescriptors::process(const SpectrumFrame& spec,
                                float                overall_rms,
                                bool                 any_event,
                                uint32_t             now_ms) {
    // -- Centroid: pure per-frame function of the spectrum. ----------
    centroid_ = compute_centroid_u8(spec);

    // -- Energy: smoothed RMS, log-normalised. -----------------------
    //
    // Single-pole IIR on the linear RMS gives a multi-second
    // envelope; subsequent log-normalisation collapses the wide
    // dynamic range of typical music into a useful 0..255 surface.
    energy_smoothed_ = (1.0f - kEnergyAlpha) * energy_smoothed_
                     + kEnergyAlpha * overall_rms;
    if (energy_smoothed_ <= 0.0f) {
        energy_ = 0;
    } else {
        const float log_v = std::log2(energy_smoothed_);
        const float frac  = (log_v - kEnergyLog2Floor) / kEnergyLog2Span;
        energy_ = clamp_to_u8(frac * 255.0f);
    }

    // -- Density: events-per-second windowed over kDensityWindowMs. --
    //
    // Push the current event (if any) into the ring buffer, then
    // count entries with timestamps within (now - kDensityWindowMs).
    if (any_event) {
        event_ts_[event_head_] = now_ms;
        event_head_ = (event_head_ + 1) % kDensityRingCap;
        if (event_count_ < kDensityRingCap) ++event_count_;
    }

    const uint32_t window_start = (now_ms > kDensityWindowMs)
                                    ? now_ms - kDensityWindowMs : 0;
    size_t in_window = 0;
    for (size_t i = 0; i < event_count_; ++i) {
        if (event_ts_[i] >= window_start) ++in_window;
    }

    // events-per-second = in_window * 1000 / window. Use the actual
    // window length so early-boot density (when we've only collected
    // partial-window data) isn't artificially low.
    const float window_ms_f = (now_ms < kDensityWindowMs)
                              ? static_cast<float>(now_ms == 0 ? 1 : now_ms)
                              : static_cast<float>(kDensityWindowMs);
    const float eps = static_cast<float>(in_window) * 1000.0f / window_ms_f;
    density_ = clamp_to_u8(eps / kDensityMaxEvents * 255.0f);
}

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
