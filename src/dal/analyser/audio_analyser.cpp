#include "dal/analyser/audio_analyser.h"

#include <cmath>
#include <cstdint>

namespace nocturnation {
namespace dal {
namespace analyser {

namespace {

// Map an FFT bin index to its centre frequency in Hz.
inline float bin_freq_hz(size_t bin, uint32_t sample_rate_hz, size_t n_bins) {
    return static_cast<float>(bin) *
           (static_cast<float>(sample_rate_hz) / (2.0f * static_cast<float>(n_bins)));
}

}  // namespace

void compute_band_summaries(const float*  magnitudes,
                            size_t        n_bins,
                            uint32_t      sample_rate_hz,
                            BandSummary8& out_8band,
                            BandSummary3& out_3band) {
    out_8band = {0, 0, 0, 0, 0, 0, 0, 0};

    if (magnitudes == nullptr || n_bins == 0 || sample_rate_hz == 0) {
        out_3band = {0, 0, 0};
        return;
    }

    // Walk every FFT bin once, route its magnitude to the correct
    // perceptual band by frequency. The chained if-else cascade is
    // sorted from low to high frequency; once a bin matches a band
    // the rest of the cascade is skipped for that bin.
    //
    // Bin 0 (DC) and any bin whose centre lands above the Air band's
    // 20 kHz ceiling are silently dropped. At Nyquist <= 20 kHz the
    // Air ceiling clamp is benign (nothing exists above Nyquist
    // anyway); at Nyquist > 20 kHz (future operating points) it
    // honestly truncates the perceptual surface at the perceptual
    // ceiling.
    for (size_t i = 0; i < n_bins; ++i) {
        const float f = bin_freq_hz(i, sample_rate_hz, n_bins);
        const float m = magnitudes[i];

        if      (f < PerceptualBoundsHz::mud_hi)        out_8band.mud       += m;
        else if (f < PerceptualBoundsHz::sub_bass_hi)   out_8band.sub_bass  += m;
        else if (f < PerceptualBoundsHz::bass_hi)       out_8band.bass      += m;
        else if (f < PerceptualBoundsHz::low_mids_hi)   out_8band.low_mids  += m;
        else if (f < PerceptualBoundsHz::midrange_hi)   out_8band.midrange  += m;
        else if (f < PerceptualBoundsHz::high_mids_hi)  out_8band.high_mids += m;
        else if (f < PerceptualBoundsHz::presence_hi)   out_8band.presence  += m;
        else if (f < PerceptualBoundsHz::air_hi)        out_8band.air       += m;
        // else: above 20 kHz - dropped (perceptual ceiling)
    }

    // 3-band roll-up: a strict aggregation of 8-band so consumers
    // that read either surface see consistent energies. Computing
    // it here rather than from the 3-band cascade rules out drift.
    out_3band.bass   = out_8band.mud + out_8band.sub_bass + out_8band.bass;
    out_3band.mid    = out_8band.low_mids + out_8band.midrange;
    out_3band.treble = out_8band.high_mids + out_8band.presence + out_8band.air;
}

void compute_spectrum_frame(const float*    magnitudes,
                            size_t          n_bins,
                            uint32_t        sample_rate_hz,
                            SpectrumFrame&  out) {
    for (size_t i = 0; i < kSpectrumBands; ++i) out.magnitudes[i] = 0.0f;

    if (magnitudes == nullptr || n_bins == 0 || sample_rate_hz == 0) return;

    // Spectrum frame covers [kSpectrumLoHz, Nyquist) as 32 log-spaced
    // bands. Each band's frequency boundaries are
    //   f_lo[i] = kSpectrumLoHz * (Nyquist / kSpectrumLoHz)^(i / 32)
    //   f_hi[i] = kSpectrumLoHz * (Nyquist / kSpectrumLoHz)^((i + 1) / 32)
    // computed once via constant ratio per band.
    const float nyquist = static_cast<float>(sample_rate_hz) * 0.5f;
    if (nyquist <= kSpectrumLoHz) return;  // pathological: no audible range

    const float ratio_per_band = std::pow(nyquist / kSpectrumLoHz,
                                          1.0f / static_cast<float>(kSpectrumBands));

    // Walk bins, assign to the appropriate spectrum band by frequency.
    // For each bin compute log-position relative to kSpectrumLoHz and
    // divide by log(ratio_per_band) to get the band index. Cheaper than
    // scanning band boundaries since we have many bins and few bands.
    const float log_ratio = std::log(ratio_per_band);
    for (size_t i = 0; i < n_bins; ++i) {
        const float f = bin_freq_hz(i, sample_rate_hz, n_bins);
        if (f < kSpectrumLoHz) continue;
        if (f >= nyquist)      continue;  // can't happen for one-sided FFT but cheap to check

        const int idx = static_cast<int>(std::log(f / kSpectrumLoHz) / log_ratio);
        if (idx < 0 || idx >= static_cast<int>(kSpectrumBands)) continue;
        out.magnitudes[idx] += magnitudes[i];
    }
}

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
