// Sub-band adaptive-threshold beat detector. Consumes the analyser's
// 32-band log-spaced spectrum (one frame per FFT cycle) and reports
// whether the current frame contains a beat.
//
// Algorithm (Patin 2003 / Parallelcube 2018, adapted for ESP32):
//
//   1. Maintain a rolling history of recent magnitudes per sub-band
//      (default 40 frames ≈ 1 s at 40 Hz FFT cadence).
//   2. For each frame, compute per-band running mean and variance
//      across the history window.
//   3. Fire a beat candidate when ANY watched sub-band's current
//      magnitude exceeds (mean + k × std_dev) for that band. The
//      threshold is per-band and self-calibrating - quiet bands have
//      correspondingly low thresholds, loud bands have high ones, so
//      the algorithm produces equivalent behavioural output on hosts
//      with very different microphone SNR (the cross-device
//      consistency goal of Epic 4.5).
//   4. A refractory window blocks subsequent firings until the
//      configured gap has elapsed - prevents one kick triggering
//      multiple beat events on the same envelope.
//   5. Watched sub-bands focus on the bass / kick-drum region
//      (default bands 0-10, covering ~30-215 Hz at the canonical
//      16 kHz / 512 FFT operating point). Bands above this are
//      computed but not watched for kick onsets - that's where snare
//      and hi-hat detection in Epic 4.7 will plug in via separate
//      detectors over the same spectrum.
//
// Why "candidate" rather than "beat" in step 3: only the first
// candidate within the refractory window fires; subsequent candidates
// are suppressed. The detector exposes is_beat() returning the
// post-refractory result.
//
// This class is pure (no globals, no HAL). It runs identically in
// the firmware loop on Plus2 / S3 / future hosts and in native test
// builds against synthetic spectrum vectors.

#pragma once

#include "dal/analyser/audio_analyser.h"

#include <cstdint>
#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

// History window in frames. Fixed at compile time so the per-band
// ring buffer can be statically allocated; this matters on the Plus2
// where dynamic allocation in the audio path would risk fragmentation.
// 40 frames ≈ 1 second at 40 Hz FFT cadence.
constexpr size_t kBeatDetectorHistorySize = 40;

// Number of low-frequency sub-bands watched for beat candidates by
// default. Indices 0-10 of the spectrum frame span ~30-215 Hz at the
// canonical 16 kHz / 512 FFT operating point - the kick-drum and
// bass-fundamental region. Higher-frequency bands (snare, hi-hat)
// are consumed by Epic 4.7's separate onset detectors over the same
// spectrum.
constexpr size_t kBeatDetectorDefaultWatchCount = 11;

// Tuning knobs for BeatDetector. Defined as a sibling rather than a
// nested type so default member initialisers can reference the
// kBeatDetector* constants without the nested-class chicken-and-egg
// the C++ standard catches at parse time.
struct BeatDetectorConfig {
    // Number of low-frequency sub-bands to watch. Indices 0..N-1.
    // Watch count up to kSpectrumBands is permitted; exceeding it is
    // clamped at construction time.
    size_t   watch_count   = kBeatDetectorDefaultWatchCount;

    // Threshold multiplier. A frame's per-band magnitude must exceed
    // (mean + k × std_dev) of the history to flag the band as a
    // candidate. Typical range 1.0 - 2.0; default 1.5 tunes mid-range
    // across the reference samples (kick drum at 60-150 BPM, Vengaboys,
    // ambient room noise).
    float    threshold_k   = 1.5f;

    // Refractory period in milliseconds. After a beat fires, no
    // subsequent beat can fire until this gap elapses. 80 ms is tight
    // enough to track 240 BPM (250 ms inter-beat interval) while
    // preventing one kick's envelope from triggering twice.
    uint32_t refractory_ms = 80;

    // Minimum frames before the detector starts firing. Until the
    // history is at least this full, mean and variance are unreliable
    // so we suppress all candidates rather than fire false positives
    // at boot. Default 8 frames (~200 ms warm-up).
    size_t   warmup_frames = 8;
};

class BeatDetector {
public:
    // Re-exposed so the values are reachable through the class name
    // for consumers that already have a BeatDetector type in scope.
    static constexpr size_t kHistorySize       = kBeatDetectorHistorySize;
    static constexpr size_t kDefaultWatchCount = kBeatDetectorDefaultWatchCount;

    using Config = BeatDetectorConfig;

    explicit BeatDetector(const Config& cfg = Config{});

    // Feed one spectrum frame. Returns true if a beat fires this
    // frame (i.e. at least one watched band exceeded its adaptive
    // threshold AND the refractory window has elapsed).
    //
    // now_ms is the frame's wall time, used to track the refractory
    // window. Pass millis() in firmware; pass a synthetic timestamp
    // in tests.
    bool process(const SpectrumFrame& frame, uint32_t now_ms);

    // Clear all history and the refractory clock. Used at mode
    // transitions (entering Master mode, leaving Pause) so a stale
    // history doesn't bias detection on resume.
    void reset();

    // Read-only diagnostics.
    size_t   frames_seen() const { return frames_seen_; }
    uint32_t last_beat_ms() const { return last_beat_ms_; }

private:
    Config cfg_;

    // Per-band ring buffer of recent magnitudes. history_[band][i] is
    // the (i)th most-recent magnitude for the band. Size is fixed at
    // compile time so this struct fits in BSS without dynamic alloc.
    float    history_[kSpectrumBands][kBeatDetectorHistorySize];
    size_t   history_idx_   = 0;          // next slot to write across all bands
    size_t   frames_seen_   = 0;          // capped at kHistorySize for stats
    uint32_t last_beat_ms_  = 0;
};

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
