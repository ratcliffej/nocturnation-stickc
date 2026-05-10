#include "dal/analyser/audio_analyser.h"

#include <cmath>
#include <cstdint>

namespace nocturnation {
namespace dal {
namespace analyser {

namespace {

// Map an FFT bin index to its centre frequency in Hz.
//
// Byte-identical equivalent of the original
//   bin * (sample_rate_hz / (2 * n_bins))
// kept here as a fallback path for inputs the LUT cache hasn't been
// primed for. The LUT cache (bin_freq_lut_for) is the hot path; this
// fallback is only ever hit for the first call at a given operating
// point or if the LUT capacity is exhausted (kBinFreqLutMaxBins).
inline float bin_freq_hz(size_t bin, uint32_t sample_rate_hz, size_t n_bins) {
    return static_cast<float>(bin) *
           (static_cast<float>(sample_rate_hz) / (2.0f * static_cast<float>(n_bins)));
}

// Bin -> Hz LUT cache. Populated lazily on the first call at a given
// (sample_rate_hz, n_bins) operating point and reused across all
// subsequent frames at that operating point. In practice the HAL
// backends pin one operating point for the lifetime of the process
// (Plus2: 16 kHz / 512 FFT; S3: same), so this cache is primed on
// the first audio frame and then read-only.
//
// Layout: lut[i] = float(i) * (sample_rate_hz / (2 * n_bins)) - the
// SAME expression order as bin_freq_hz() above, so the LUT entries
// are bitwise identical to what the per-call computation would
// produce. This preserves the byte-parity guarantee that downstream
// consumers (PixMob wire output via BeatDetector) depend on.
//
// kBinFreqLutMaxBins caps the static buffer. 1024 covers FFT sizes
// up to 2048 (n_bins = fft_size / 2), which is the largest operating
// point we expect a host to declare. If a future host declares a
// larger FFT, the LUT path falls back to per-call bin_freq_hz()
// silently rather than allocating dynamically; the analyser still
// produces correct output, just without the LUT speed-up.
constexpr size_t kBinFreqLutMaxBins = 1024;

struct BinFreqLutCache {
    uint32_t sample_rate_hz = 0;  // 0 sentinel: cache cold
    size_t   n_bins         = 0;
    float    values[kBinFreqLutMaxBins];
};

// Function-local static so the cache lives in BSS rather than .data
// and so initialisation is deferred until first call (no global-
// constructor cost at firmware boot).
inline BinFreqLutCache& bin_freq_lut_cache() {
    static BinFreqLutCache cache;
    return cache;
}

// Return a pointer to a primed bin -> Hz LUT for the given operating
// point, or nullptr if n_bins exceeds the static capacity (caller
// then falls back to bin_freq_hz()). The cache is rebuilt only when
// the operating point changes; the common case (steady operating
// point) is a single uint32_t + size_t comparison.
inline const float* bin_freq_lut_for(uint32_t sample_rate_hz, size_t n_bins) {
    if (n_bins > kBinFreqLutMaxBins) return nullptr;
    BinFreqLutCache& cache = bin_freq_lut_cache();
    if (cache.sample_rate_hz != sample_rate_hz || cache.n_bins != n_bins) {
        // Compute once. The expression below is the same one bin_freq_hz()
        // uses, evaluated in the same order, so cache entries are bitwise
        // identical to what bin_freq_hz() would return.
        const float step = static_cast<float>(sample_rate_hz) /
                           (2.0f * static_cast<float>(n_bins));
        for (size_t i = 0; i < n_bins; ++i) {
            cache.values[i] = static_cast<float>(i) * step;
        }
        cache.sample_rate_hz = sample_rate_hz;
        cache.n_bins         = n_bins;
    }
    return cache.values;
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

    // Prime / fetch the bin -> Hz LUT for this operating point. Falls
    // back to per-call bin_freq_hz() if n_bins exceeds LUT capacity.
    const float* bin_hz = bin_freq_lut_for(sample_rate_hz, n_bins);

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
        const float f = bin_hz ? bin_hz[i] : bin_freq_hz(i, sample_rate_hz, n_bins);
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

    // Hoist the transcendentals to a one-shot cache keyed on the
    // operating point. ratio_per_band and log_ratio depend only on
    // sample_rate_hz (Nyquist = sample_rate_hz / 2; kSpectrumLoHz and
    // kSpectrumBands are compile-time constants), so a steady-state
    // operating point computes std::pow / std::log exactly once and
    // reuses the result for every subsequent frame.
    //
    // Cached values are the bitwise result of the same expressions
    // the original per-frame code used (same operand order, same
    // float precision), so byte-parity with the pre-cache behaviour
    // is preserved.
    struct SpectrumScalarCache {
        uint32_t sample_rate_hz = 0;
        float    ratio_per_band = 0.0f;
        float    log_ratio      = 0.0f;
    };
    static SpectrumScalarCache scalar_cache;
    if (scalar_cache.sample_rate_hz != sample_rate_hz) {
        scalar_cache.ratio_per_band = std::pow(nyquist / kSpectrumLoHz,
                                               1.0f / static_cast<float>(kSpectrumBands));
        scalar_cache.log_ratio      = std::log(scalar_cache.ratio_per_band);
        scalar_cache.sample_rate_hz = sample_rate_hz;
    }
    // ratio_per_band is stored in the cache for symmetry with the
    // pre-cache code's named locals and to make the cache's purpose
    // self-documenting; only log_ratio is consumed below.
    const float log_ratio = scalar_cache.log_ratio;

    // Prime / fetch the bin -> Hz LUT for this operating point. Falls
    // back to per-call bin_freq_hz() if n_bins exceeds LUT capacity.
    const float* bin_hz = bin_freq_lut_for(sample_rate_hz, n_bins);

    // Walk bins, assign to the appropriate spectrum band by frequency.
    // For each bin compute log-position relative to kSpectrumLoHz and
    // divide by log(ratio_per_band) to get the band index. Cheaper than
    // scanning band boundaries since we have many bins and few bands.
    for (size_t i = 0; i < n_bins; ++i) {
        const float f = bin_hz ? bin_hz[i] : bin_freq_hz(i, sample_rate_hz, n_bins);
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
