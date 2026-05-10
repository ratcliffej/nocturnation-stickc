#include "dal/analyser/drop_detector.h"

namespace nocturnation {
namespace dal {
namespace analyser {

DropDetector::DropDetector(const Config& cfg) : cfg_(cfg) {
    if (cfg_.short_window_frames == 0 ||
        cfg_.short_window_frames > kDropDetectorMaxShortWindow) {
        cfg_.short_window_frames = kDropDetectorMaxShortWindow;
    }
    if (cfg_.long_window_frames == 0 ||
        cfg_.long_window_frames > kDropDetectorMaxLongWindow) {
        cfg_.long_window_frames = kDropDetectorMaxLongWindow;
    }
    reset();
}

void DropDetector::reset() {
    for (size_t i = 0; i < kDropDetectorMaxShortWindow; ++i) short_buf_[i] = 0.0f;
    for (size_t i = 0; i < kDropDetectorMaxLongWindow;  ++i) long_buf_[i]  = 0.0f;
    short_idx_           = 0;
    long_idx_            = 0;
    frames_seen_         = 0;
    last_event_ms_       = 0;
    armed_for_drop_      = true;
    armed_for_breakdown_ = true;
}

DropEvent DropDetector::process(float bass_energy, uint32_t now_ms) {
    // Phase 1: push into both ring buffers. We do this BEFORE the
    // ratio check so the very first frame's energy contributes to
    // the means - otherwise the first ratio computation would divide
    // by an empty long buffer.
    short_buf_[short_idx_] = bass_energy;
    long_buf_ [long_idx_ ] = bass_energy;
    short_idx_ = (short_idx_ + 1) % cfg_.short_window_frames;
    long_idx_  = (long_idx_  + 1) % cfg_.long_window_frames;
    if (frames_seen_ < cfg_.long_window_frames) ++frames_seen_;

    // Phase 2: gate on warm-up. The long window needs to be at least
    // partially populated for the baseline to mean anything.
    if (frames_seen_ < cfg_.warmup_frames) return DropEvent::None;

    // Phase 3: gate on cooldown. Any event in the last cooldown_ms
    // suppresses subsequent events.
    if (last_event_ms_ != 0 && (now_ms - last_event_ms_) < cfg_.cooldown_ms) {
        return DropEvent::None;
    }

    // Phase 4: compute means. The active window is min(frames_seen_,
    // window_frames) - early on we use the partial window so the ratio
    // is meaningful before the buffers fill.
    const size_t s_window = (frames_seen_ < cfg_.short_window_frames)
                          ? frames_seen_ : cfg_.short_window_frames;
    const size_t l_window = (frames_seen_ < cfg_.long_window_frames)
                          ? frames_seen_ : cfg_.long_window_frames;

    float s_sum = 0.0f;
    for (size_t i = 0; i < s_window; ++i) s_sum += short_buf_[i];
    const float s_mean = s_sum / static_cast<float>(s_window);

    float l_sum = 0.0f;
    for (size_t i = 0; i < l_window; ++i) l_sum += long_buf_[i];
    const float l_mean = l_sum / static_cast<float>(l_window);

    // Phase 5: ratio comparison. Guard against division-by-zero on
    // a totally silent long window - if the song has been silent for
    // 10 seconds and a frame of energy arrives, that's a "fire DROP"
    // signal in spirit but the ratio is undefined; we treat it as
    // None and let the next frame, when l_mean > 0, decide.
    if (l_mean <= 0.0f) return DropEvent::None;
    const float ratio = s_mean / l_mean;

    // Re-arm logic: a drop only re-arms after the ratio falls back to
    // baseline (≤ 1.0); a breakdown only re-arms after the ratio
    // rises back above baseline. This makes both events fire on
    // genuine *transitions* rather than on the persistent post-
    // transition state, so a sustained chorus doesn't keep re-firing
    // DROP every cooldown cycle.
    if (ratio <= 1.0f) armed_for_drop_      = true;
    if (ratio >= 1.0f) armed_for_breakdown_ = true;

    DropEvent event = DropEvent::None;
    if (ratio > cfg_.drop_ratio && armed_for_drop_) {
        event           = DropEvent::Drop;
        armed_for_drop_ = false;
    } else if (ratio < cfg_.breakdown_ratio && armed_for_breakdown_) {
        event                = DropEvent::Breakdown;
        armed_for_breakdown_ = false;
    }

    if (event != DropEvent::None) last_event_ms_ = now_ms;
    return event;
}

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
