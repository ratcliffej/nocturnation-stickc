// SectionDetector implementation (Epic 4.7 Block 4).
//
// State machine over centroid / energy / density IIRs plus the Epic
// 4.5 DropDetector's drop event flag. See section_detector.h for the
// architecture rationale (slow/fast IIRs in place of a literal
// rolling-history buffer) and section taxonomy.

#include "dal/analyser/section_detector.h"

namespace nocturnation {
namespace dal {
namespace analyser {

SectionDetector::SectionDetector(const Config& cfg) : cfg_(cfg) {
    reset();
}

void SectionDetector::reset() {
    centroid_iir_           = SignalIIR{};
    energy_iir_             = SignalIIR{};
    density_iir_            = SignalIIR{};
    frames_seen_            = 0;
    current_                = SectionType::Unknown;
    candidate_              = SectionType::Unknown;
    candidate_streak_       = 0;
    breakdown_streak_       = 0;
    drop_hold_remaining_    = 0;
}

SectionType SectionDetector::evaluate_candidate() const {
    // Priority order (highest first):
    //   1. DROP (handled outside this function via drop_hold_remaining_)
    //   2. BREAKDOWN (sustained low energy + low density)
    //   3. BUILDUP (rising energy AND density slopes)
    //   4. CHORUS (high level energy + density)
    //   5. VERSE  (mid level energy)
    //   6. UNKNOWN
    //
    // The BREAKDOWN sustain check happens at the caller (it tracks the
    // breakdown streak); here we just identify whether the current
    // frame's IIR state qualifies as "in breakdown territory".

    const float energy_level   = energy_iir_.level();
    const float density_level  = density_iir_.level();
    const float energy_slope   = energy_iir_.slope();
    const float density_slope  = density_iir_.slope();

    const float rising_slope = static_cast<float>(cfg_.rising_slope);

    // Build-up: both energy and density visibly rising. Centroid often
    // rises too but isn't strictly required (drum-driven build-ups can
    // have low centroid throughout).
    if (energy_slope > rising_slope && density_slope > rising_slope) {
        return SectionType::BuildUp;
    }

    // Chorus: high sustained energy + density.
    if (energy_level  >= static_cast<float>(cfg_.energy_high)
     && density_level >= static_cast<float>(cfg_.density_high)) {
        return SectionType::Chorus;
    }

    // Verse: mid energy, low-to-mid density. We bracket against the
    // low thresholds; below the low thresholds is breakdown territory
    // which the caller's sustain check decides on.
    if (energy_level  >= static_cast<float>(cfg_.energy_low)
     && density_level <  static_cast<float>(cfg_.density_high)) {
        return SectionType::Verse;
    }

    return SectionType::Unknown;
}

SectionType SectionDetector::process(uint8_t centroid,
                                      uint8_t energy,
                                      uint8_t density,
                                      bool    drop_event_fired) {
    // Update IIRs.
    centroid_iir_.update(static_cast<float>(centroid),
                          cfg_.fast_alpha, cfg_.slow_alpha);
    energy_iir_.update  (static_cast<float>(energy),
                          cfg_.fast_alpha, cfg_.slow_alpha);
    density_iir_.update (static_cast<float>(density),
                          cfg_.fast_alpha, cfg_.slow_alpha);

    ++frames_seen_;

    // Drop latch: external event takes priority and holds for
    // drop_hold_frames so the IIRs catch up before falling back to
    // whatever the descriptors say.
    if (drop_event_fired) {
        drop_hold_remaining_ = cfg_.drop_hold_frames;
    }
    if (drop_hold_remaining_ > 0) {
        --drop_hold_remaining_;
        current_          = SectionType::Drop;
        candidate_        = SectionType::Drop;
        candidate_streak_ = 0;
        // BREAKDOWN streak doesn't accumulate during DROP either - the
        // drop's bass spike doesn't qualify as breakdown territory.
        breakdown_streak_ = 0;
        return current_;
    }

    if (frames_seen_ < cfg_.warmup_frames) {
        return current_;   // stays Unknown
    }

    // BREAKDOWN sustain tracking: streak frames where the recent
    // (fast IIR, ~0.5 s time constant) energy AND density both sit
    // below their low thresholds. The slow IIR is too lagged to spot
    // a transition into breakdown within the spec's 2 s sustain
    // window - by the time slow drops below the threshold the
    // breakdown is already half over. Using the fast IIR for this
    // check gives the streak room to fill while still rejecting
    // single-frame dips through noise. The slow IIR remains the
    // signal for VERSE / CHORUS level decisions where lag is desired.
    const bool below_low_floor =
        energy_iir_.fast  < static_cast<float>(cfg_.energy_low) &&
        density_iir_.fast < static_cast<float>(cfg_.density_low);
    if (below_low_floor) {
        if (breakdown_streak_ < cfg_.breakdown_sustain_frames) {
            ++breakdown_streak_;
        }
    } else {
        breakdown_streak_ = 0;
    }

    SectionType candidate;
    if (breakdown_streak_ >= cfg_.breakdown_sustain_frames) {
        candidate = SectionType::Breakdown;
    } else {
        candidate = evaluate_candidate();
    }

    // Hysteresis: only commit a transition once the candidate has
    // held for transition_frames consecutive frames. BREAKDOWN
    // already has its own sustain check so it can transition
    // immediately once the streak fills.
    if (candidate == current_) {
        candidate_        = current_;
        candidate_streak_ = 0;
    } else if (candidate == candidate_) {
        ++candidate_streak_;
        if (candidate_streak_ >= cfg_.transition_frames
            || candidate == SectionType::Breakdown) {
            current_          = candidate;
            candidate_streak_ = 0;
        }
    } else {
        candidate_        = candidate;
        candidate_streak_ = 1;
        // Single-frame BREAKDOWN qualifier - the sustain check above
        // already guards against premature firing, so allow the
        // transition immediately here.
        if (candidate == SectionType::Breakdown) {
            current_          = candidate;
            candidate_streak_ = 0;
        }
    }

    return current_;
}

}  // namespace analyser
}  // namespace dal
}  // namespace nocturnation
