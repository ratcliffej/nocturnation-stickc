// Transport-agnostic signal quality tracker.
//
// Counts received-vs-expected frames per source from sequence numbers and
// maps the rolling-window loss percentage onto a 0-4 bar quality indicator.
// Any sequenced protocol whose sender increments per-frame can use this -
// ESP-NOW LIGHT_COMMAND/HEARTBEAT today, future IR ack protocols and BLE
// links plug in the same way.
//
// "Quality" rather than "strength": tracks delivered fidelity (frames
// received vs frames expected) which is what a show operator cares about.
// Strong RSSI with 30% packet loss is a bad show; weak RSSI with 0% loss
// is fine. This mirrors the latter, and crucially does so without requiring
// promiscuous-mode WiFi sniffing - which would be miserable in dense
// venues like EMF.
//
// Sequence-number conventions follow architecture spec §4.3:
//   - 1-255 wrapping (255 -> 1, 0 reserved as "sequencing disabled")
//   - 0 frames are skipped for quality purposes
//   - Source ID change (e.g. master swap) resets state cleanly

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace transport {

class SignalQuality {
public:
    // Loss-percentage thresholds for the bar mapping. Tuned for show-
    // coordination perception: 4 bars = essentially lossless, 0 bars =
    // catastrophic.
    //   <  5 %  -> 4 bars (excellent)
    //   <  15 % -> 3 bars (good)
    //   <  30 % -> 2 bars (workable)
    //   <  50 % -> 1 bar  (poor)
    //   >= 50 % -> 0 bars (broken)
    static constexpr uint16_t kThresholdPctExcellent = 5;
    static constexpr uint16_t kThresholdPctGood      = 15;
    static constexpr uint16_t kThresholdPctWorkable  = 30;
    static constexpr uint16_t kThresholdPctPoor      = 50;

    // Wait until at least this many frames have been observed before
    // returning a real bar count - smaller samples are too noisy to
    // map meaningfully. Callers that want an indicator earlier should
    // fall back to a freshness-based proxy when bars() returns -1.
    static constexpr uint16_t kMinFramesForBars = 3;

    // Rolling-window length in milliseconds. When this elapses the
    // received and missed counters are halved, giving an exponentially-
    // weighted recent estimate without unbounded counter growth.
    static constexpr uint32_t kWindowMs = 5000;

    // Record one received frame from the given source. seq=0 frames are
    // ignored (per spec, 0 means sequencing disabled); duplicates should
    // be filtered by the caller's dedup before reaching here.
    void note_frame(uint8_t source_id, uint8_t sequence_number,
                    uint32_t now_ms);

    // Reset all state (mode entry, explicit "start fresh", etc.).
    void reset();

    // 0-4 bar count. Returns -1 if not enough data yet (caller should
    // fall back to a freshness/age proxy).
    int bars(uint32_t now_ms) const;

    // Diagnostic accessors.
    uint16_t received_count() const { return received_count_; }
    uint16_t missed_count()   const { return missed_count_;   }
    uint8_t  current_source() const { return last_source_id_; }

private:
    uint8_t  last_source_id_  = 0;
    uint8_t  last_seq_        = 0;     // 0 = no frame seen for current source
    uint16_t received_count_  = 0;
    uint16_t missed_count_    = 0;
    uint32_t window_start_ms_ = 0;
};

}  // namespace transport
}  // namespace nocturnation
