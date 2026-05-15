// NocturNation Effect class hierarchy.
//
// An Effect is the unit of artistic intent that orchestration produces and
// drivers translate to hardware (per architecture spec §6). This header
// declares the Effect base class and the four primitives marked
// "Implemented" in spec §6.1: Pulse, Probability Pulse, Rainbow / Hue
// Cycle, and Starlight. The six remaining primitives (Random Palette
// Pulse, Wave, Gradient Hold, Strobe Burst, Background Wash, Two-Colour
// Flash) are spec'd but deferred.
//
// Effects fire IR through DAL::fire_rgb_pulse against a target device name
// supplied at construction. The "device type + group_id" addressing model
// is a deferred Epic 2 design question; until Epic 4 brings a real second
// device type to pressure-test against, target-by-name is sufficient.
//
// Lifetime: orchestration (typically a Mode) owns one or more Effect
// instances. enter()/exit() bracket the time an effect is active.
// Beat-driven effects (Pulse, ProbabilityPulse) react in on_beat();
// continuous effects (Rainbow, Starlight) tick in loop_tick().

#pragma once

#include <cstdint>
#include "dal/dal.h"

namespace nocturnation {
namespace effects {

// =============================================================================
// HSV -> RGB utility
// =============================================================================
//
// Used by Rainbow; exposed here so future palette-based effects can share it.
// Inputs: h in [0, 360), s and v in [0, 1]. Outputs: r/g/b in [0, 255].

void hsv_to_rgb(float h, float s, float v,
                uint8_t& r, uint8_t& g, uint8_t& b);

// =============================================================================
// Effect base class
// =============================================================================

class Effect {
public:
    virtual ~Effect() = default;
    virtual const char* name() const = 0;

    // Lifecycle hooks. Default no-ops; concrete classes override what they
    // need (e.g. Starlight seeds its RNG in enter()).
    virtual void enter() {}
    virtual void exit()  {}

    // Continuous effects use loop_tick. now_ms is the current millis(); the
    // effect tracks deltas internally.
    virtual void loop_tick(uint32_t /*now_ms*/) {}

    // Beat-driven effects use on_beat. now_ms is the current millis() at the
    // moment the beat detector fired; bpm is the most recent BPM estimate
    // (0 if not yet known).
    virtual void on_beat(uint32_t /*now_ms*/, float /*bpm*/) {}

    // Optional: raw audio frames, in case an effect needs the spectrum
    // beyond the discrete beat events. Default no-op.
    virtual void on_audio_frame(const dal::AudioFrameEvent&) {}
};

// =============================================================================
// Pulse - single colour fired with ASR envelope on each detected beat
// =============================================================================
//
// The current default beat-reactive behaviour (was inline in
// DirectorMode pre-AC1). The envelope (attack / sustain / release)
// is selected at fire time based on the current BPM:
//   > 160 BPM:               very fast (0+32+96 = 128 ms)
//   100..160 / unknown BPM:  punchy default (32+96+96 = 224 ms)
//   <= 100 BPM:              slow ballad (32+192+192 = 416 ms)
//
// Colour is mutable via set_colour(); typical orchestration call site is a
// button handler that cycles through a palette.

// BPM -> attack/sustain/release envelope picker. Single source of truth for
// the prototype's tempo-driven envelope mapping; Pulse / ProbabilityPulse use
// it for IR fires, AutonomousMaster uses it when packing LIGHT_COMMAND frames
// for ESP-NOW so slaves render the same envelope on screen.
struct PulseEnvelope {
    pixmob::Time attack;
    pixmob::Time sustain;
    pixmob::Time release;
};
PulseEnvelope envelope_for_bpm(float bpm);

class Pulse : public Effect {
public:
    explicit Pulse(const char* target_name);

    const char* name() const override { return "Pulse"; }
    void on_beat(uint32_t now_ms, float bpm) override;

    void set_colour(uint8_t r, uint8_t g, uint8_t b);

protected:
    // ProbabilityPulse derives from this and reuses the BPM->envelope path
    // verbatim, only differing in the chance value passed to the encoder.
    void fire(uint8_t r, uint8_t g, uint8_t b, float bpm);

    const char*    target_;
    uint8_t        r_ = 0xFF, g_ = 0x00, b_ = 0x00;
    pixmob::Chance chance_ = pixmob::CHANCE_100;
};

// =============================================================================
// ProbabilityPulse - Pulse with per-target chance gating
// =============================================================================
//
// Each PixMob receiver rolls independently against the chance value; across
// many bracelets this produces a "popcorn" twinkle. Chance defaults to
// CHANCE_50; orchestration can pass others (CHANCE_25, CHANCE_75, etc.) to
// vary the density.

class ProbabilityPulse : public Pulse {
public:
    ProbabilityPulse(const char* target_name,
                     pixmob::Chance chance = pixmob::CHANCE_50);

    const char* name() const override { return "ProbabilityPulse"; }
};

// =============================================================================
// Rainbow / Hue Cycle - smooth HSV cycle over a duration
// =============================================================================
//
// Continuous, software-driven: at a fixed update rate (default ~20 Hz) the
// effect fires a single short-envelope RGB pulse with the next hue step.
// Does not react to beats. Brightness param is the V in HSV (0..1).
//
// cycle_speed_hz is full hue cycles per second; 1.0 = one cycle every
// second, 0.25 = a 4-second cycle, etc.

class Rainbow : public Effect {
public:
    Rainbow(const char* target_name,
            float cycle_speed_hz = 0.4f,
            float brightness     = 1.0f,
            uint16_t step_interval_ms = 50);

    const char* name() const override { return "Rainbow"; }
    void enter() override;
    void loop_tick(uint32_t now_ms) override;

private:
    const char* target_;
    float       cycle_speed_hz_;
    float       brightness_;
    uint16_t    step_interval_ms_;
    float       hue_              = 0.0f;
    uint32_t    last_step_ms_     = 0;
};

// =============================================================================
// Starlight - sparse, randomly-timed twinkles
// =============================================================================
//
// Continuous but irregular. Each tick decides whether to fire based on a
// randomised inter-pulse interval (mean +/- jitter). When firing, picks a
// colour from the palette at random and sends a low-chance, long-release
// pulse. Designed for ambient passages.
//
// Default palette is single white; warm/cool-white tinting per spec §6 is
// a future tweak (warm white isn't in the current colour palette).

class Starlight : public Effect {
public:
    Starlight(const char* target_name,
              uint16_t mean_interval_ms = 300,
              uint16_t jitter_ms        = 200,
              pixmob::Chance chance     = pixmob::CHANCE_16);

    const char* name() const override { return "Starlight"; }
    void enter() override;
    void loop_tick(uint32_t now_ms) override;

private:
    const char*     target_;
    uint16_t        mean_interval_ms_;
    uint16_t        jitter_ms_;
    pixmob::Chance  chance_;
    uint32_t        next_fire_ms_   = 0;
};

}  // namespace effects
}  // namespace nocturnation
