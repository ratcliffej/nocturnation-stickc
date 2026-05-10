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
//   5. Watched sub-bands focus on the kick-drum fundamental region
//      (default bands 0-7, covering ~30-150 Hz at the canonical
//      16 kHz / 512 FFT operating point). Bands above this are
//      computed but not watched for kick onsets - that's where snare
//      and hi-hat detection in Epic 4.7 will plug in via separate
//      detectors over the same spectrum.
//
// Tuning history (so future-you doesn't relitigate it):
//   2026-05-10 round 1: initial integration on hardware (Plus2 + S3)
//   with Vengaboys-class test track. Original defaults (k=1.5, 80 ms
//   refractory, 11 bands watched 30-215 Hz) over-fired badly: a
//   112 BPM track read 155 BPM, screen flashed semi-continuously.
//   Tightened to k=2.5 (matches legacy single-threshold's beat
//   multiplier; the maker-community 1.5 is per-band-only and
//   compounds with band count), 200 ms refractory (allows up to
//   300 BPM, prevents same-kick double-fire as the envelope decays
//   through neighbouring sub-bands), and 8 watched bands focused
//   on the kick fundamental at 30-150 Hz.
//   2026-05-10 round 2: cross-device hardware verification on Plus2
//   + S3 showed the round-1 tuning was just slightly too tight - a
//   few softer kicks were being missed, leaving >1 s gaps that the
//   master's heartbeat (architecture spec §4.3) legitimately filled.
//   Mechanism: after a loud kick the kick's energy enters the per-
//   band history, elevating the mean and raising the threshold for
//   the next ~1 s. A soft kick within that window can fall just
//   below the elevated threshold and miss. Loosened k from 2.5 to
//   2.2 - 10% more permissive, picks up the soft-after-loud kicks
//   without re-introducing the round-1 over-fire. A future structural
//   improvement (outlier-rejecting mean: exclude individual loud
//   spikes from contributing to the history's mean while still
//   counting them for variance) would close this loop more cleanly
//   but is out of Epic 4.5 scope.
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
// default. Indices 0-7 of the spectrum frame span ~30-150 Hz at the
// canonical 16 kHz / 512 FFT operating point - the kick-drum
// fundamental range for pop / rock / dance / drum & bass. Sub-bass
// (band 0-1) and lower-mid (bands 8-10) are read into history but
// not watched; including them in the watched set is what caused
// initial over-firing on hardware (Jason's Vengaboys test, 2026-05-10)
// where a 112 BPM track read out at ~155 BPM. Each watched band
// independently risks firing on its own variance, so the false-
// positive probability compounds with watch count - keep it tight.
//
// Higher-frequency onsets (snare ~200-2k Hz, hi-hat ~5-8k Hz) get
// their own dedicated detectors in Epic 4.7 rather than competing
// for kick-band attention here.
constexpr size_t kBeatDetectorDefaultWatchCount = 8;

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
    // candidate. Typical range 2.0 - 3.0; default 2.2 settles the
    // sensitivity between the maker-community 1.5 (over-fires on
    // multi-band watching) and the legacy single-threshold detector's
    // 2.5 (misses softer kicks that follow a loud kick within ~1 s,
    // because the loud kick's history elevates the per-band mean).
    // 2.2 catches the soft kicks without re-introducing the over-
    // fire seen at 1.5 - confirmed empirically on Jason's hardware
    // tests with Plus2 + S3 (2026-05-10).
    float    threshold_k   = 2.2f;

    // Refractory period in milliseconds. After a beat fires, no
    // subsequent beat can fire until this gap elapses. 200 ms allows
    // up to 300 BPM (well above the 4 Hz / 240 BPM safety cap in
    // architecture §15.1) and crucially prevents a single kick's
    // envelope from re-triggering as it decays through different
    // sub-bands - the failure mode that produced 155-BPM readouts
    // on a 112-BPM track during Jason's hardware test (2026-05-10).
    uint32_t refractory_ms = 200;

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
