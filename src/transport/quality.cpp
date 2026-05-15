#include "transport/quality.h"

namespace nocturnation {
namespace transport {

void SignalQuality::note_frame(uint8_t source_id, uint8_t seq,
                               uint32_t now_ms) {
    // Sequencing-disabled frames (per spec §4.3) carry no information
    // about delivery - skip.
    if (seq == 0) return;

    // New source: reset state and seed with this first frame. Avoids
    // counting "missed" frames from a previous Director that just went away.
    if (last_source_id_ != source_id) {
        last_source_id_  = source_id;
        last_seq_        = seq;
        received_count_  = 1;
        missed_count_    = 0;
        window_start_ms_ = now_ms;
        return;
    }

    // Window decay first: halve the running counters when the window
    // expires so the estimate stays recent without unbounded growth.
    // Round received up so it never falls to zero while frames are still
    // arriving. Decay BEFORE counting the new frame, so this frame
    // contributes its full weight to the next window rather than being
    // immediately halved away.
    if (now_ms - window_start_ms_ >= kWindowMs) {
        received_count_  = static_cast<uint16_t>((received_count_ + 1) / 2);
        missed_count_    = static_cast<uint16_t>(missed_count_ / 2);
        window_start_ms_ = now_ms;
    }

    // Same source. Compute the gap, accounting for the wrap from 255 -> 1
    // (per spec, sequence 0 is skipped). Only count missed frames on
    // forward progression; treat wrap or out-of-order as zero missed
    // (defensive - we under-count by at most one frame per 254 around
    // wrap, acceptable inaccuracy).
    if (last_seq_ != 0 && seq > last_seq_) {
        missed_count_ += static_cast<uint16_t>(seq - last_seq_ - 1);
    }
    received_count_++;
    last_seq_ = seq;
}

void SignalQuality::reset() {
    last_source_id_  = 0;
    last_seq_        = 0;
    received_count_  = 0;
    missed_count_    = 0;
    window_start_ms_ = 0;
}

int SignalQuality::bars(uint32_t /*now_ms*/) const {
    if (received_count_ < kMinFramesForBars) return -1;
    const uint32_t total = static_cast<uint32_t>(received_count_)
                         + missed_count_;
    if (total == 0) return -1;
    const uint16_t loss_pct =
        static_cast<uint16_t>((static_cast<uint32_t>(missed_count_) * 100)
                              / total);
    if (loss_pct < kThresholdPctExcellent) return 4;
    if (loss_pct < kThresholdPctGood)      return 3;
    if (loss_pct < kThresholdPctWorkable)  return 2;
    if (loss_pct < kThresholdPctPoor)      return 1;
    return 0;
}

}  // namespace transport
}  // namespace nocturnation
