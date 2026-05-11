#include "dal/analyser/beat_detector.h"

#include <cmath>

namespace nocturnation {
namespace dal {
namespace analyser {

BeatDetector::BeatDetector(const Config& cfg) : cfg_(cfg) {
    if (cfg_.watch_count == 0)                cfg_.watch_count = kDefaultWatchCount;
    if (cfg_.watch_start >= kSpectrumBands)   cfg_.watch_start = 0;
    if (cfg_.watch_start + cfg_.watch_count > kSpectrumBands) {
        cfg_.watch_count = kSpectrumBands - cfg_.watch_start;
    }
    reset();
}

void BeatDetector::reset() {
    for (size_t b = 0; b < kSpectrumBands; ++b) {
        for (size_t i = 0; i < kHistorySize; ++i) history_[b][i] = 0.0f;
    }
    history_idx_   = 0;
    frames_seen_   = 0;
    last_beat_ms_  = 0;
    last_strength_ = 0;
}

bool BeatDetector::process(const SpectrumFrame& frame, uint32_t now_ms) {
    // Phase 1: compute mean and standard deviation per watched band
    // across the existing history. Frames seen so far is the divisor;
    // before kHistorySize we use a partial window. After it we always
    // use kHistorySize since the ring buffer overwrites in place.
    const size_t window = (frames_seen_ < kHistorySize) ? frames_seen_ : kHistorySize;
    bool fired = false;

    if (window >= cfg_.warmup_frames) {
        // Refractory check: any beat candidate during the refractory
        // window is suppressed. Done before per-band evaluation since
        // it short-circuits the whole frame.
        const bool in_refractory =
            (last_beat_ms_ != 0) && ((now_ms - last_beat_ms_) < cfg_.refractory_ms);

        if (!in_refractory) {
            const size_t b_end = cfg_.watch_start + cfg_.watch_count;
            for (size_t b = cfg_.watch_start; b < b_end; ++b) {
                // Single-pass mean + variance via Welford's online
                // algorithm. Combines the two history walks (one for
                // mean, one for sum-of-squared-deviations) into a
                // single pass, halving the per-band memory traffic
                // on the audio hot path. We still report sample
                // variance (M2 / window) rather than unbiased
                // (M2 / (window - 1)) so the result matches the
                // pre-Welford behaviour, sub-LSB float reordering
                // aside.
                //
                // Welford recurrence (Knuth TAOCP vol 2 / Welford 1962):
                //   for each x_i in the window (i = 1..window):
                //     n     = i
                //     delta = x_i - mean
                //     mean += delta / n
                //     M2   += delta * (x_i - mean_new)
                //   variance_population = M2 / window
                //
                // Output is variance / std_dev numerically equivalent
                // to the prior two-pass form within float precision;
                // the existing beat-detector tests are the regression
                // gate (TEST_ASSERT_EQUAL_FLOAT, tolerance well above
                // sub-LSB reordering noise).
                float mean = 0.0f;
                float m2   = 0.0f;
                for (size_t i = 0; i < window; ++i) {
                    const float x     = history_[b][i];
                    const float delta = x - mean;
                    mean += delta / static_cast<float>(i + 1);
                    const float delta2 = x - mean;
                    m2 += delta * delta2;
                }
                const float variance = m2 / static_cast<float>(window);
                const float std_dev  = std::sqrt(variance);

                const float threshold = mean + cfg_.threshold_k * std_dev;
                if (frame.magnitudes[b] > threshold) {
                    fired = true;
                    // Strength: linear over the magnitude-vs-threshold
                    // ratio. mag == threshold reads 0; mag == 2*threshold
                    // reads ~128; mag >= 3*threshold saturates at 255.
                    if (threshold > 0.0f) {
                        float s = (frame.magnitudes[b] / threshold - 1.0f) * 128.0f;
                        if (s < 0.0f)    s = 0.0f;
                        if (s > 255.0f)  s = 255.0f;
                        last_strength_ = static_cast<uint8_t>(s);
                    } else {
                        last_strength_ = 255;   // degenerate: no baseline yet
                    }
                    break;  // any watched band exceeding threshold fires the beat
                }
            }
        }
    }

    // Phase 2: write current frame's magnitudes into history. We do
    // this AFTER the threshold check so the current frame doesn't
    // bias the mean/variance it's being compared against. Otherwise
    // a sudden spike would partially mask itself.
    for (size_t b = 0; b < kSpectrumBands; ++b) {
        history_[b][history_idx_] = frame.magnitudes[b];
    }
    history_idx_ = (history_idx_ + 1) % kHistorySize;
    if (frames_seen_ < kHistorySize) ++frames_seen_;

    if (fired) last_beat_ms_ = now_ms;
    return fired;
}

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
