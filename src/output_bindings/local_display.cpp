// LocalDisplayBinding - implementation.
//
// Epic 4.6 Block 9 origin: extracted from src/modes/lume_mode.cpp's
// render_light() first render_fx call as a thin forwarder.
// Epic 6C Phase F: WASH-family rendering owned by the binding directly.
//   - When no wash is active, on_light_pulse forwards to render_fx("local",
//     ev) exactly as before (LocalDriver renders the pulse animation).
//   - When a wash IS active, the binding's tick() takes over the LCD:
//     paints the wash baseline (cosine-eased ping-pong between r1g1b1 and
//     r2g2b2 over cycle_ms, or held r1g1b1 when cycle_ms == 0), with any
//     active pulse additively blended on top.
// Wire output of LIGHT_PULSE without an active wash is byte-identical to
// pre-Phase-F.

#include "output_bindings/local_display.h"
#include "output_bindings/output_binding_context.h"

#include "dal/dal.h"
#include "pulse/envelope.h"   // pulse::Time enum values for envelope-ms lookup

#include <cmath>
#include <cstdint>

namespace nocturnation {
namespace output_bindings {

using namespace nocturnation::dal;
using nocturnation::plugins::PropertyBag;

namespace {

// ---------------------------------------------------------------------------
// Helpers (file-static)
// ---------------------------------------------------------------------------

// Wash attack/release are u8 in 100 ms units (range 0..25.5 s).
constexpr uint32_t units_to_ms(uint8_t units) {
    return static_cast<uint32_t>(units) * 100u;
}

// pulse::Time enum index -> milliseconds. Order mirrors the enumeration.
constexpr uint16_t kPulseTimeMs[8] = { 0, 32, 96, 192, 480, 960, 2400, 3840 };
inline uint16_t pulse_time_ms(uint8_t idx) {
    return kPulseTimeMs[idx & 0x07];
}

// 8-8-8 -> 5-6-5 (LCD framebuffer encoding used by DisplayClearEvent).
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

inline uint8_t clip255(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

inline uint8_t lerp8(uint8_t a, uint8_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return static_cast<uint8_t>(static_cast<float>(a)
                                + (static_cast<float>(b) - static_cast<float>(a)) * t);
}

// Compute the instantaneous wash baseline (already intensity-scaled) given
// the wash params and now. Drift is cosine-eased ping-pong over cycle_ms
// when non-zero; otherwise hold r1g1b1.
inline void compute_wash_baseline(uint8_t  r1, uint8_t g1, uint8_t b1,
                                  uint8_t  r2, uint8_t g2, uint8_t b2,
                                  uint16_t cycle_ms,
                                  uint8_t  intensity,
                                  uint32_t started_ms,
                                  uint32_t now_ms,
                                  uint8_t& out_r,
                                  uint8_t& out_g,
                                  uint8_t& out_b) {
    float drift_r = static_cast<float>(r1);
    float drift_g = static_cast<float>(g1);
    float drift_b = static_cast<float>(b1);
    if (cycle_ms != 0) {
        const float phase = 2.0f * static_cast<float>(M_PI)
                          * static_cast<float>(now_ms - started_ms)
                          / static_cast<float>(cycle_ms);
        const float t = 0.5f - 0.5f * cosf(phase);
        drift_r = static_cast<float>(r1) + (static_cast<float>(r2) - static_cast<float>(r1)) * t;
        drift_g = static_cast<float>(g1) + (static_cast<float>(g2) - static_cast<float>(g1)) * t;
        drift_b = static_cast<float>(b1) + (static_cast<float>(b2) - static_cast<float>(b1)) * t;
    }
    const float scale = static_cast<float>(intensity) / 255.0f;
    out_r = clip255(static_cast<int>(drift_r * scale));
    out_g = clip255(static_cast<int>(drift_g * scale));
    out_b = clip255(static_cast<int>(drift_b * scale));
}

}  // namespace

// =============================================================================
// LocalDisplayBinding
// =============================================================================

hal::CapabilityMask LocalDisplayBinding::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Display);
}

// ----------------------------------------------------------------------------
// PULSE - forwards to LocalDriver when no wash is active (preserves the
// pre-Phase-F path byte-for-byte). Sets up an additive-overlay pulse when
// a wash is active and pulse_response == 1; silently drops when
// pulse_response == 0.
// ----------------------------------------------------------------------------

void LocalDisplayBinding::on_light_pulse(OutputBindingContext& /*ctx*/,
                                          const RgbPulseEvent& ev) {
    if (wash_phase_ == WashPhase::Inactive
        || wash_phase_ == WashPhase::Releasing) {
        // No live wash baseline to overlay on - let LocalDriver animate
        // the pulse as before. Releasing is treated as no-wash for the
        // purposes of pulse handling - the operator is on the way out.
        DAL::render_fx("local", ev);
        return;
    }
    // Active wash. Respect pulse_response.
    if (wash_pulse_response_ == 0) {
        return;   // drop silently; wash holds untouched
    }
    pulse_active_     = true;
    pulse_started_ms_ = 0;   // stamp on first tick; we don't have now_ms here
    pulse_r_          = ev.r;
    pulse_g_          = ev.g;
    pulse_b_          = ev.b;
    pulse_total_ms_   = pulse_time_ms(static_cast<uint8_t>(ev.attack))
                      + pulse_time_ms(static_cast<uint8_t>(ev.sustain))
                      + pulse_time_ms(static_cast<uint8_t>(ev.release));
}

// ----------------------------------------------------------------------------
// WASH - enter (or refresh) wash state. A second on_light_wash supersedes:
// we re-stamp wash_started_ms (cosine phase resets), capture the new
// params, and run the attack lerp from the current rendered colour.
// ----------------------------------------------------------------------------

void LocalDisplayBinding::on_light_wash(OutputBindingContext& /*ctx*/,
                                         const transport::espnow::LightWashPayload& p) {
    // Capture the colour we're currently rendering as the attack-lerp
    // source. On a cold start the binding's pre_wash_* default to black.
    // For a supersede, we capture the *instantaneous* wash baseline.
    if (wash_phase_ != WashPhase::Inactive) {
        compute_wash_baseline(wash_r1_, wash_g1_, wash_b1_,
                              wash_r2_, wash_g2_, wash_b2_,
                              wash_cycle_ms_, wash_intensity_,
                              wash_started_ms_, /*now_ms=*/0,
                              pre_wash_r_, pre_wash_g_, pre_wash_b_);
    }

    wash_r1_ = p.r1; wash_g1_ = p.g1; wash_b1_ = p.b1;
    wash_r2_ = p.r2; wash_g2_ = p.g2; wash_b2_ = p.b2;
    wash_attack_units_    = p.attack;
    wash_release_units_   = p.release;
    wash_intensity_       = p.intensity;
    wash_cycle_ms_        = p.cycle_ms;
    wash_ttl_seconds_     = p.ttl_seconds;
    wash_pulse_response_  = p.pulse_response;
    wash_phase_           = WashPhase::Attacking;
    // wash_started_ms_ + wash_phase_started_ms_ are stamped in the next
    // tick() call (we don't have a millis() reference here without a
    // context arg; the design's "stamp on receipt" is OK with a 1-tick
    // lag - ~50 ms - and keeps the binding HAL-clock-free here).
    wash_started_ms_       = 0;
    wash_phase_started_ms_ = 0;
}

void LocalDisplayBinding::on_light_wash_end(OutputBindingContext& /*ctx*/,
                                             uint8_t release_time) {
    if (wash_phase_ == WashPhase::Inactive) return;
    // Capture current rendered colour as the fade-source by treating it
    // as the new pre-wash baseline; the fade goes from "wherever we are
    // right now" to black.
    compute_wash_baseline(wash_r1_, wash_g1_, wash_b1_,
                          wash_r2_, wash_g2_, wash_b2_,
                          wash_cycle_ms_, wash_intensity_,
                          wash_started_ms_, /*now_ms=*/0,
                          pre_wash_r_, pre_wash_g_, pre_wash_b_);
    release_units_active_ = release_time;
    release_end_r_        = 0;
    release_end_g_        = 0;
    release_end_b_        = 0;
    wash_phase_            = WashPhase::Releasing;
    wash_phase_started_ms_ = 0;
}

void LocalDisplayBinding::on_light_wash_pulse(OutputBindingContext& /*ctx*/,
                                               const RgbPulseEvent& ev) {
    // Same rules as the design doc: drop if no active wash; otherwise
    // accept as additive overlay regardless of pulse_response (the
    // separate message type IS the explicit "I want this overlay").
    if (wash_phase_ != WashPhase::Attacking
        && wash_phase_ != WashPhase::Holding) {
        return;
    }
    pulse_active_     = true;
    pulse_started_ms_ = 0;
    pulse_r_          = ev.r;
    pulse_g_          = ev.g;
    pulse_b_          = ev.b;
    pulse_total_ms_   = pulse_time_ms(static_cast<uint8_t>(ev.attack))
                      + pulse_time_ms(static_cast<uint8_t>(ev.sustain))
                      + pulse_time_ms(static_cast<uint8_t>(ev.release));
}

// ----------------------------------------------------------------------------
// tick - drives the wash phase machine and paints the screen each loop.
// When the wash is Inactive the tick is a near-no-op (LocalDriver handles
// pulse rendering via the render_fx path).
// ----------------------------------------------------------------------------

void LocalDisplayBinding::tick(OutputBindingContext& /*ctx*/, uint32_t now_ms) {
    if (wash_phase_ == WashPhase::Inactive) return;

    // First tick after on_light_wash / on_light_wash_end: stamp the
    // wash_started / phase_started timestamps now that we have a clock.
    if (wash_started_ms_ == 0)        wash_started_ms_       = now_ms;
    if (wash_phase_started_ms_ == 0)  wash_phase_started_ms_ = now_ms;
    if (pulse_active_ && pulse_started_ms_ == 0) pulse_started_ms_ = now_ms;

    // -- Compute the instantaneous wash baseline (post-intensity). -------
    uint8_t base_r = 0, base_g = 0, base_b = 0;
    compute_wash_baseline(wash_r1_, wash_g1_, wash_b1_,
                          wash_r2_, wash_g2_, wash_b2_,
                          wash_cycle_ms_, wash_intensity_,
                          wash_started_ms_, now_ms,
                          base_r, base_g, base_b);

    // -- Phase machine. ----------------------------------------------------
    uint8_t out_r = base_r, out_g = base_g, out_b = base_b;
    switch (wash_phase_) {
        case WashPhase::Attacking: {
            const uint32_t attack_ms = units_to_ms(wash_attack_units_);
            if (attack_ms == 0) {
                wash_phase_ = WashPhase::Holding;
                wash_phase_started_ms_ = now_ms;
            } else {
                const uint32_t elapsed = now_ms - wash_phase_started_ms_;
                const float t = static_cast<float>(elapsed)
                              / static_cast<float>(attack_ms);
                out_r = lerp8(pre_wash_r_, base_r, t);
                out_g = lerp8(pre_wash_g_, base_g, t);
                out_b = lerp8(pre_wash_b_, base_b, t);
                if (elapsed >= attack_ms) {
                    wash_phase_ = WashPhase::Holding;
                    wash_phase_started_ms_ = now_ms;
                }
            }
            break;
        }
        case WashPhase::Holding: {
            // Apply TTL expiry: transition to Releasing using the wash's
            // own release as the fade duration.
            if (wash_ttl_seconds_ != 0) {
                const uint32_t ttl_ms = static_cast<uint32_t>(wash_ttl_seconds_) * 1000u;
                if (now_ms - wash_started_ms_ >= ttl_ms) {
                    pre_wash_r_           = base_r;
                    pre_wash_g_           = base_g;
                    pre_wash_b_           = base_b;
                    release_units_active_ = wash_release_units_;
                    release_end_r_        = 0;
                    release_end_g_        = 0;
                    release_end_b_        = 0;
                    wash_phase_            = WashPhase::Releasing;
                    wash_phase_started_ms_ = now_ms;
                }
            }
            // out_{r,g,b} already hold the live baseline.
            break;
        }
        case WashPhase::Releasing: {
            const uint32_t release_ms = units_to_ms(release_units_active_);
            if (release_ms == 0) {
                out_r = release_end_r_;
                out_g = release_end_g_;
                out_b = release_end_b_;
                wash_phase_ = WashPhase::Inactive;
                pulse_active_ = false;
            } else {
                const uint32_t elapsed = now_ms - wash_phase_started_ms_;
                const float t = static_cast<float>(elapsed)
                              / static_cast<float>(release_ms);
                out_r = lerp8(pre_wash_r_, release_end_r_, t);
                out_g = lerp8(pre_wash_g_, release_end_g_, t);
                out_b = lerp8(pre_wash_b_, release_end_b_, t);
                if (elapsed >= release_ms) {
                    wash_phase_  = WashPhase::Inactive;
                    pulse_active_ = false;
                }
            }
            break;
        }
        default: break;
    }

    // -- Apply pulse overlay on top of the wash output (additive). --------
    if (pulse_active_ && pulse_total_ms_ > 0
        && (wash_phase_ == WashPhase::Attacking || wash_phase_ == WashPhase::Holding)) {
        const uint32_t elapsed = now_ms - pulse_started_ms_;
        if (elapsed >= pulse_total_ms_) {
            pulse_active_ = false;
        } else {
            // Triangular envelope: ramps to full at midpoint, fades to 0.
            // A linear fade-back to baseline is the simpler approximation
            // of the ASR shape; visually indistinguishable on an LCD at
            // these timescales and avoids dragging in pixmob::Time per-
            // segment arithmetic.
            const float t = static_cast<float>(elapsed)
                          / static_cast<float>(pulse_total_ms_);
            const float strength = (t < 0.5f) ? (t * 2.0f)
                                              : (1.0f - (t - 0.5f) * 2.0f);
            out_r = clip255(static_cast<int>(out_r) + static_cast<int>(static_cast<float>(pulse_r_) * strength));
            out_g = clip255(static_cast<int>(out_g) + static_cast<int>(static_cast<float>(pulse_g_) * strength));
            out_b = clip255(static_cast<int>(out_b) + static_cast<int>(static_cast<float>(pulse_b_) * strength));
        }
    }

    // -- Paint the LCD. Whole screen, single colour per tick. -------------
    DAL::fire_display_clear("local", DisplayClearEvent{ rgb565(out_r, out_g, out_b) });
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
LocalDisplayBinding  s_instance;
PropertyBag          s_bag(s_instance);
OutputBindingContext s_ctx(s_instance, s_bag);
}  // namespace

LocalDisplayBinding*  local_display_instance()     { return &s_instance; }
PropertyBag&          local_display_property_bag() { return s_bag; }
OutputBindingContext& local_display_context()      { return s_ctx; }

}  // namespace output_bindings
}  // namespace nocturnation
