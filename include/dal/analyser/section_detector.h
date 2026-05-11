// SectionDetector - longer-window state machine identifying song-
// structure events (Epic 4.7 Block 4).
//
// Consumes the per-frame descriptors from MusicDescriptors (centroid,
// energy, density) plus the Epic 4.5 DropDetector's drop event flag,
// and emits a SectionType - the current section label.
//
// Architecture: rather than a literal rolling-history buffer of every
// (c, e, d) sample, the detector tracks two single-pole IIRs per
// signal at different time constants ("fast" ~1 s and "slow" ~5 s at
// 40 Hz FFT cadence). The fast minus slow gives a directional slope
// (positive = rising over the window) and the slow gives a long-term
// level. That's enough to identify build-ups, choruses, verses, and
// breakdowns without keeping ~200 floats of history per signal.
//
// Hysteresis: a candidate section must hold for kTransitionFrames
// consecutive frames before the detector commits the transition. This
// suppresses spurious switching when the signals dip across a
// boundary briefly.
//
// Section taxonomy:
//   0  UNKNOWN            - defaults until enough data accumulates, or
//                           when no rule matches.
//   1  VERSE              - mid energy, mid centroid, low-to-mid density.
//   2  CHORUS             - high energy, high density.
//   3  BUILDUP            - all three signals rising together over the
//                           window.
//   4  BREAKDOWN          - low energy + low density sustained ~2 s.
//   5  VOCALS_ONLY        - reserved for future (needs per-band data).
//   6  INSTRUMENTAL_BREAK - reserved for future.
//   7  DROP               - latched briefly after the DropDetector
//                           fires a DROP event (Epic 4.5).
//
// Block 4 implements UNKNOWN / VERSE / CHORUS / BUILDUP / BREAKDOWN /
// DROP. VOCALS_ONLY and INSTRUMENTAL_BREAK are declared in the enum
// for forward-compat but always stay UNKNOWN - distinguishing them
// requires per-band energy that isn't surfaced to this detector.
//
// Pure analyser class: no HAL, no globals. Tests drive it with
// synthetic descriptor sequences and assert section labels.

#pragma once

#include <cstdint>
#include <cstddef>

namespace nocturnation {
namespace dal {
namespace analyser {

enum class SectionType : uint8_t {
    Unknown           = 0,
    Verse             = 1,
    Chorus            = 2,
    BuildUp           = 3,
    Breakdown         = 4,
    VocalsOnly        = 5,
    InstrumentalBreak = 6,
    Drop              = 7,
};

struct SectionDetectorConfig {
    // Smoothing alphas. "fast" tracks the recent half-window
    // (~1 s @ 40 Hz); "slow" tracks the long-term mean (~5 s @ 40 Hz).
    // Slope = fast - slow gives a directional trend.
    float    fast_alpha = 0.05f;     // 1 / (1/0.05) = 20 frames ~ 0.5 s @ 40 Hz
    float    slow_alpha = 0.01f;     // 100 frames ~ 2.5 s @ 40 Hz

    // Level thresholds. Energy and density are 0..255 from
    // MusicDescriptors; "low" / "high" partition the level surface.
    uint8_t  energy_low   = 64;      // ~25 % - silence-ish floor
    uint8_t  energy_high  = 160;     // ~63 % - chorus floor
    uint8_t  density_low  = 32;
    uint8_t  density_high = 96;

    // Rising-slope threshold. fast minus slow exceeding this on
    // energy AND density signals a build-up. Centroid rise often
    // coincides but isn't strictly required (drum-driven build-ups
    // can have low centroid throughout).
    uint8_t  rising_slope = 16;      // ~6 % of the 0..255 surface

    // Hysteresis: candidate state must hold for this many consecutive
    // frames before transitioning. 8 frames ≈ 200 ms @ 40 Hz.
    size_t   transition_frames = 8;

    // Breakdown sustain: low energy + low density must hold for at
    // least this many frames before BREAKDOWN latches. 80 frames ≈
    // 2 s @ 40 Hz - the architecture's BREAKDOWN definition.
    size_t   breakdown_sustain_frames = 80;

    // Drop hold: after a DropDetector DROP fires, hold SectionType::Drop
    // for this many frames so consumers see the section even if the
    // detector's slow IIRs lag the actual transition. 40 frames ≈ 1 s.
    size_t   drop_hold_frames = 40;

    // Minimum frames before the detector starts emitting non-Unknown
    // sections. Lets the slow IIR settle to a stable level. 80 frames
    // ≈ 2 s @ 40 Hz.
    size_t   warmup_frames = 80;
};

class SectionDetector {
public:
    using Config = SectionDetectorConfig;

    explicit SectionDetector(const Config& cfg = Config{});

    // Run one update for the current frame. centroid / energy /
    // density are the u8 values from MusicDescriptors. drop_event_fired
    // is true on the frame the Epic 4.5 DropDetector fired a DROP.
    // Returns the section after this frame's update.
    SectionType process(uint8_t centroid,
                         uint8_t energy,
                         uint8_t density,
                         bool    drop_event_fired);

    SectionType current() const { return current_; }

    // Reset all smoothing + hysteresis state.
    void reset();

private:
    Config cfg_;

    // Per-signal IIRs. Stored as floats for sub-LSB drift; the
    // descriptors live in 0..255 so values stay in a tight range.
    struct SignalIIR {
        float fast = 0.0f;
        float slow = 0.0f;
        void update(float x, float fa, float sa) {
            fast = fa * x + (1.0f - fa) * fast;
            slow = sa * x + (1.0f - sa) * slow;
        }
        float slope() const { return fast - slow; }
        float level() const { return slow; }
    };
    SignalIIR centroid_iir_;
    SignalIIR energy_iir_;
    SignalIIR density_iir_;

    size_t   frames_seen_              = 0;
    SectionType current_               = SectionType::Unknown;
    SectionType candidate_             = SectionType::Unknown;
    size_t   candidate_streak_         = 0;
    size_t   breakdown_streak_         = 0;
    size_t   drop_hold_remaining_      = 0;

    // Compute the candidate section from current IIR state.
    SectionType evaluate_candidate() const;
};

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
