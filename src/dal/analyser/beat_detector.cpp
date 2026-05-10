#include "dal/analyser/beat_detector.h"

#include <cmath>

namespace nocturnation {
namespace dal {
namespace analyser {

BeatDetector::BeatDetector(const Config& cfg) : cfg_(cfg) {
    if (cfg_.watch_count == 0)                cfg_.watch_count = kDefaultWatchCount;
    if (cfg_.watch_count > kSpectrumBands)    cfg_.watch_count = kSpectrumBands;
    reset();
}

void BeatDetector::reset() {
    for (size_t b = 0; b < kSpectrumBands; ++b) {
        for (size_t i = 0; i < kHistorySize; ++i) history_[b][i] = 0.0f;
    }
    history_idx_  = 0;
    frames_seen_  = 0;
    last_beat_ms_ = 0;
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
            for (size_t b = 0; b < cfg_.watch_count; ++b) {
                // Mean across the window.
                float sum = 0.0f;
                for (size_t i = 0; i < window; ++i) sum += history_[b][i];
                const float mean = sum / static_cast<float>(window);

                // Variance, then std dev. We use sample variance
                // (divide by window) rather than unbiased (window-1)
                // because at small windows the bias correction is
                // negligible compared to the FFT noise floor and the
                // simpler form avoids a divide-by-zero edge case.
                float sq_diff_sum = 0.0f;
                for (size_t i = 0; i < window; ++i) {
                    const float d = history_[b][i] - mean;
                    sq_diff_sum += d * d;
                }
                const float variance = sq_diff_sum / static_cast<float>(window);
                const float std_dev  = std::sqrt(variance);

                const float threshold = mean + cfg_.threshold_k * std_dev;
                if (frame.magnitudes[b] > threshold) {
                    fired = true;
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
