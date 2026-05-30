// NocturNation Effect implementations.
//
// All four Epic 2 effects live here. If any single one grows past ~150
// lines, lift it into its own file.

#include "effects/effects.h"

#include <cmath>
#include <cstdlib>

namespace nocturnation {
namespace effects {

using namespace nocturnation::dal;

// =============================================================================
// HSV -> RGB
// =============================================================================
//
// Standard chroma-based conversion. Inputs h in [0, 360); s, v in [0, 1].
// Negative h or h >= 360 are wrapped into range first.

void hsv_to_rgb(float h, float s, float v,
                uint8_t& r, uint8_t& g, uint8_t& b) {
    h = std::fmod(h, 360.0f);
    if (h < 0) h += 360.0f;

    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;

    float r1, g1, b1;
    if      (h < 60.0f)  { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300.0f) { r1 = x; g1 = 0; b1 = c; }
    else                 { r1 = c; g1 = 0; b1 = x; }

    r = (uint8_t)((r1 + m) * 255.0f);
    g = (uint8_t)((g1 + m) * 255.0f);
    b = (uint8_t)((b1 + m) * 255.0f);
}

// =============================================================================
// Pulse
// =============================================================================

Pulse::Pulse(const char* target_name)
    : target_(target_name) {}

void Pulse::set_colour(uint8_t r, uint8_t g, uint8_t b) {
    r_ = r; g_ = g; b_ = b;
}

void Pulse::on_beat(uint32_t /*now_ms*/, float bpm) {
    // Skip if colour is full black; matches Director's MODE_OFF
    // behaviour pre-AC1 (cycle includes an "OFF" colour that suppresses IR).
    if (r_ == 0 && g_ == 0 && b_ == 0) return;
    fire(r_, g_, b_, bpm);
}

PulseEnvelope envelope_for_bpm(float bpm) {
    if (bpm > 160.0f) {
        // Very fast: 0+32+96 = 128 ms total.
        return { pulse::T_0_MS,  pulse::T_32_MS,  pulse::T_96_MS  };
    }
    if (bpm > 100.0f || bpm == 0.0f) {
        // Medium / unknown: punchy default (32+96+96 = 224 ms).
        return { pulse::T_32_MS, pulse::T_96_MS,  pulse::T_96_MS  };
    }
    // Slow ballad: more presence (32+192+192 = 416 ms).
    return { pulse::T_32_MS, pulse::T_192_MS, pulse::T_192_MS };
}

void Pulse::fire(uint8_t r, uint8_t g, uint8_t b, float bpm) {
    const PulseEnvelope env = envelope_for_bpm(bpm);
    DAL::fire_rgb_pulse(target_, RgbPulseEvent{
        r, g, b, env.attack, env.sustain, env.release, chance_});
}

// =============================================================================
// ProbabilityPulse
// =============================================================================

ProbabilityPulse::ProbabilityPulse(const char* target_name,
                                   pulse::Chance chance)
    : Pulse(target_name) {
    chance_ = chance;     // overrides Pulse's CHANCE_100 default
}

// =============================================================================
// Rainbow
// =============================================================================

Rainbow::Rainbow(const char* target_name,
                 float cycle_speed_hz,
                 float brightness,
                 uint16_t step_interval_ms)
    : target_           (target_name),
      cycle_speed_hz_   (cycle_speed_hz),
      brightness_       (brightness),
      step_interval_ms_ (step_interval_ms) {}

void Rainbow::enter() {
    hue_          = 0.0f;
    last_step_ms_ = 0;
}

void Rainbow::loop_tick(uint32_t now_ms) {
    if (now_ms - last_step_ms_ < step_interval_ms_) return;
    last_step_ms_ = now_ms;

    // Advance hue. degrees-per-step = 360 * cycle_speed_hz / steps_per_sec.
    // steps_per_sec = 1000 / step_interval_ms_.
    const float steps_per_sec = 1000.0f / (float)step_interval_ms_;
    const float deg_per_step  = 360.0f * cycle_speed_hz_ / steps_per_sec;
    hue_ = std::fmod(hue_ + deg_per_step, 360.0f);

    uint8_t r, g, b;
    hsv_to_rgb(hue_, 1.0f, brightness_, r, g, b);

    // Envelope: attack=0, sustain=96, release=0. Hardware testing showed
    // that any non-zero release leaves a visible fade-to-dark gap between
    // commands - the next pulse arrives partway into the previous one's
    // release rather than during sustain, and the eye reads that as
    // discrete flashes rather than a hue cycle. Setting release=0 means
    // the bracelet stays at full brightness for the whole sustain window
    // and the next command's instant attack swaps colour cleanly.
    //
    // The matching constraint is on step_interval_ms: it must be short
    // enough that the next command lands during the previous sustain.
    // Each PixMob command is ~35ms on the wire (9 bytes * 8 bits *
    // PULSE_US=694us, worst case 50ms via the encoder), so step_interval
    // must be < sustain - wire_delay to keep overlap. Default 50ms paired
    // with sustain=96ms gives ~46ms of overlap; chosen short to keep the
    // hue step small (faster update rate -> smaller degrees-per-step ->
    // less visibly distinctive colour transitions). PixMob has no
    // cross-fade primitive between colours, so visual smoothness is
    // entirely a function of how many small hue increments we can fit
    // into a cycle.
    DAL::fire_rgb_pulse(target_, RgbPulseEvent{
        r, g, b,
        pulse::T_0_MS, pulse::T_96_MS, pulse::T_0_MS,
        pulse::CHANCE_100});
}

// =============================================================================
// Starlight
// =============================================================================

namespace {

// Default palette: single white. Spec §6 calls for cool/warm-white tinting,
// but warm-white isn't in the active firmware palette yet - file as a future
// tweak when palette support lands.
constexpr struct { uint8_t r, g, b; } kStarlightPalette[] = {
    {0xFF, 0xFF, 0xFF},
};
constexpr size_t kStarlightPaletteCount =
    sizeof(kStarlightPalette) / sizeof(kStarlightPalette[0]);

uint16_t random_interval(uint16_t mean, uint16_t jitter) {
    if (jitter == 0) return mean;
    const int delta = (std::rand() % (2 * jitter + 1)) - jitter;  // [-jitter, +jitter]
    int v = (int)mean + delta;
    if (v < 1) v = 1;
    return (uint16_t)v;
}

}  // namespace

Starlight::Starlight(const char* target_name,
                     uint16_t mean_interval_ms,
                     uint16_t jitter_ms,
                     pulse::Chance chance)
    : target_           (target_name),
      mean_interval_ms_ (mean_interval_ms),
      jitter_ms_        (jitter_ms),
      chance_           (chance) {}

void Starlight::enter() {
    // Schedule the first fire one interval into the future. Don't seed
    // std::rand() here - the firmware's RNG seed is whatever the caller
    // arranged (typically once at boot via randomSeed(analogRead(...)) or
    // similar). Seeding inside enter() would defeat that.
    next_fire_ms_ = 0;
}

void Starlight::loop_tick(uint32_t now_ms) {
    if (next_fire_ms_ == 0) {
        next_fire_ms_ = now_ms + random_interval(mean_interval_ms_, jitter_ms_);
        return;
    }
    if (now_ms < next_fire_ms_) return;

    const auto& c = kStarlightPalette[std::rand() % kStarlightPaletteCount];
    // Long release for a fading-star quality. T_32_MS + T_32_MS + T_192_MS
    // = 256 ms total, with most of that in the fade.
    DAL::fire_rgb_pulse(target_, RgbPulseEvent{
        c.r, c.g, c.b,
        pulse::T_32_MS, pulse::T_32_MS, pulse::T_192_MS,
        chance_});

    next_fire_ms_ = now_ms + random_interval(mean_interval_ms_, jitter_ms_);
}

}  // namespace effects
}  // namespace nocturnation
