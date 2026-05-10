// Drop and breakdown detector. Consumes a per-frame bass-energy
// scalar and fires macro-level musical events (DROP, BREAKDOWN) when
// the song's energy character shifts significantly above or below
// its medium-term baseline.
//
// Algorithm (Patin 2003 long-window pass, plus an "armed" gate so
// sustained high or low energy doesn't re-fire on cooldown expiry):
//
//   1. Maintain two ring buffers of recent bass-energy values:
//        - short window: ~2 seconds (80 frames at 40 Hz cadence)
//        - long window:  ~10 seconds (400 frames)
//   2. Each frame, push the new bass-energy onto both windows and
//      compute the means.
//   3. Compute ratio = short_mean / long_mean.
//   4. ratio > drop_ratio (default 1.8) AND drop is armed: fire DROP.
//      Disarm drop until ratio falls back below 1.0 (return to or
//      below baseline). This makes DROP a transition event rather
//      than a persistent-state event - sustained chorus energy
//      doesn't keep re-firing as cooldowns expire.
//   5. ratio < breakdown_ratio (default 0.4) AND breakdown is armed:
//      fire BREAKDOWN. Disarm breakdown until ratio returns above 1.0.
//   6. After any fire, the cooldown (default 4 s) blocks subsequent
//      fires of EITHER kind - prevents a noisy transition firing both
//      DROP and BREAKDOWN in rapid succession across a single edge.
//
// The arm/disarm logic combined with the cooldown produces "fire once
// per genuine transition" semantics: DROP fires when the song really
// shifts up, stays silent during the sustained chorus, and re-arms
// during the next quiet section ready to fire on the next chorus.
//
// Why a scalar input rather than a SpectrumFrame: drops manifest as
// broadband bass-region energy shifts, well-captured by the 3-band
// summary's bass channel (<250 Hz). Taking a scalar keeps the
// detector orthogonal to the band-layout decision: future Epics that
// add custom layouts (Epic 4.5 `set_band_layout` direction) feed
// whatever bass-equivalent energy they produce. Same algorithm, no
// spectrum-bin coupling.
//
// Why "build" event isn't fired here: a build is the rising-energy
// run-up that precedes a drop, not the drop itself. Detecting the
// build PEAK reliably needs centroid + onset-density tracking that
// isn't in this Epic's scope. Reserved as event_type 3 in the wire
// protocol (architecture spec §4.3) so receivers can forward-compat
// it; firing it lands in Epic 4.7's section-detection state machine.

#pragma once

#include <cstdint>
#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

// Wire-compatible event ids per architecture spec §4.3 MUSIC_EVENT
// payload byte. None = 0 means "no event this frame"; 1/2/3 match
// the protocol-level event_type field in MUSIC_EVENT frames.
enum class DropEvent : uint8_t {
    None      = 0,
    Drop      = 1,
    Breakdown = 2,
    Build     = 3,   // reserved; not fired by Epic 4.5
};

// Window sizes are upper bounds. The detector accepts smaller config
// values; larger values up to these caps are statically allocated.
constexpr size_t kDropDetectorMaxShortWindow = 80;    // ~2 s at 40 Hz
constexpr size_t kDropDetectorMaxLongWindow  = 400;   // ~10 s at 40 Hz

struct DropDetectorConfig {
    // Short window length in frames. Default 80 ≈ 2 seconds at 40 Hz
    // FFT cadence - tracks the song's "right now" energy.
    size_t   short_window_frames = 80;

    // Long window length in frames. Default 400 ≈ 10 seconds - the
    // medium-term baseline the short window is compared against.
    // Longer windows make the detector slower to recognise sustained
    // shifts as the new normal, which is the right behaviour: a long
    // chorus shouldn't progressively suppress further drop detection.
    size_t   long_window_frames  = 400;

    // Drop ratio. short_mean must exceed long_mean × this to fire
    // DROP. 1.8 is loose enough to catch typical pop chorus drops,
    // tight enough to avoid firing on natural verse-to-bridge swells.
    float    drop_ratio          = 1.8f;

    // Breakdown ratio. short_mean must fall below long_mean × this
    // to fire BREAKDOWN. 0.4 marks a pronounced drop-out (loss of
    // bass + main rhythm); typical vocal-only sections sit around
    // 0.5-0.6 and won't fire.
    float    breakdown_ratio     = 0.4f;

    // Cooldown in milliseconds after any fire (DROP or BREAKDOWN).
    // 4 s prevents oscillation across a noisy transition where the
    // ratio crosses thresholds repeatedly within a short window.
    uint32_t cooldown_ms         = 4000;

    // Frames before the detector is willing to fire. The long window
    // needs at least this many samples for the baseline to mean
    // anything; until then we suppress all candidates. Default 80 (2 s)
    // - same as the short window, giving the detector a meaningful
    // ratio after the song has been running for a couple of seconds.
    size_t   warmup_frames       = 80;
};

class DropDetector {
public:
    using Config = DropDetectorConfig;

    explicit DropDetector(const Config& cfg = Config{});

    // Feed one frame's bass-energy scalar (typically BandSummary3.bass
    // from the analyser's per-frame output). Returns the event for
    // this frame, or DropEvent::None if no event fired.
    DropEvent process(float bass_energy, uint32_t now_ms);

    // Clear all history and the cooldown clock. Used at mode
    // transitions so a stale baseline doesn't bias detection on resume.
    void reset();

    // Read-only diagnostics.
    size_t   frames_seen() const { return frames_seen_; }
    uint32_t last_event_ms() const { return last_event_ms_; }

private:
    Config cfg_;

    // Ring buffers. Allocated at the configured cap; the active
    // window length is cfg_.{short,long}_window_frames.
    float    short_buf_[kDropDetectorMaxShortWindow];
    float    long_buf_[kDropDetectorMaxLongWindow];
    size_t   short_idx_       = 0;
    size_t   long_idx_        = 0;
    size_t   frames_seen_     = 0;          // capped at long_window_frames for stats
    uint32_t last_event_ms_   = 0;

    // Arm gates. armed_for_drop_ is reset to true when ratio falls
    // below 1.0; armed_for_breakdown_ is reset to true when ratio
    // rises above 1.0. Both start true so the first transition after
    // warm-up can fire normally.
    bool     armed_for_drop_      = true;
    bool     armed_for_breakdown_ = true;
};

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
