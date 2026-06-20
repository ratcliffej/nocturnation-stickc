// LedStripDriver - implementation (Epic 12 B2).

#include "led_strip_driver.h"

#include <cmath>
#include <cstdint>

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace dal {

using nocturnation::hal::LedStrip;

namespace {

inline uint8_t clip255(int x) {
    if (x < 0)   return 0;
    if (x > 255) return 255;
    return static_cast<uint8_t>(x);
}

inline uint8_t lerp_u8(uint8_t a, uint8_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return clip255(static_cast<int>(static_cast<float>(a)
                  + (static_cast<float>(b) - static_cast<float>(a)) * t));
}

// Convert a 100 ms-unit wire value into milliseconds. The wire field is
// uint8_t (0..255), so the range is 0..25.5 s.
inline uint32_t units100_to_ms(uint8_t units) {
    return static_cast<uint32_t>(units) * 100u;
}

// pixmob::Time enum buckets converted into ms. Used to size sparkle
// durations from the on-wire RgbPulseEvent envelope. Mirrors the
// values in include/pixmob_protocol.h - kept local here so we don't
// pull the PixMob protocol header into the LedStrip transport. If
// these drift, the PixMob parity tests in test/test_pixmob_protocol/
// would catch it (those reference the canonical bucket values).
inline uint32_t pixmob_time_to_ms(uint8_t bucket) {
    // jamesw343's reverse-engineering buckets:
    //   T_0=0, T_32=32, T_96=96, T_192=192, T_480=480, T_960=960,
    //   T_2400=2400, T_3840=3840 (ms).
    switch (bucket) {
        case 0: return 0;
        case 1: return 32;
        case 2: return 96;
        case 3: return 192;
        case 4: return 480;
        case 5: return 960;
        case 6: return 2400;
        case 7: return 3840;
        default: return 192;
    }
}

}  // namespace

// =============================================================================
// Driver lifecycle
// =============================================================================

bool LedStripDriver::begin() {
    auto* strip = active_strip();
    if (!strip) return false;
    strip->begin();
    strip->clear();
    strip->show();
    // Auto-enable the signal-state indicator when the host has no
    // Display capability. Rationale: on Atom Lite the LED strip is
    // the only visible affordance, and "the firmware booted and is
    // searching" needs a pilot light. On StickC Plus2 / S3 the LCD
    // already shows mode + status, so the indicator stays off there
    // to keep the strip render clean. Callers can override post-
    // begin via set_signal_indicator_enabled() (the future B4-live
    // Config "LED Strip" toggle).
    if (!hal::HAL::has(hal::Capability::Display)) {
        signal_indicator_enabled_ = true;
    }
    return true;
}

LedStrip* LedStripDriver::active_strip() const {
    return strip_override_ ? strip_override_ : hal::HAL::led_strip();
}

uint32_t LedStripDriver::now_ms() const {
    if (clock_source_) return clock_source_();
#ifdef ARDUINO
    return ::millis();
#else
    // Native test envs without an injected clock get a frozen zero;
    // tests that exercise time-dependent paths install a stub via
    // set_clock_source(). Mirrors PixMobIRDriver::now_ms().
    return 0;
#endif
}

uint32_t LedStripDriver::next_random() {
    if (rng_source_) return rng_source_();
    // xorshift32 - tiny, deterministic, no <random> dependency. Seeded
    // lazily from now_ms() on first call so the strip doesn't always
    // pick pixel 0 for the first sparkle of a session.
    if (rng_state_ == 0) {
        rng_state_ = now_ms() ^ 0xA5A5A5A5u;
        if (rng_state_ == 0) rng_state_ = 0xDEADBEEFu;
    }
    rng_state_ ^= rng_state_ << 13;
    rng_state_ ^= rng_state_ >> 17;
    rng_state_ ^= rng_state_ << 5;
    return rng_state_;
}

// =============================================================================
// Wash family
// =============================================================================

bool LedStripDriver::send_wash(uint8_t /*target_group*/, const LightWashEvent& ev) {
    if (!enabled() || !active_strip()) return false;

    // Signal-indicator hook: stamp last-wash-received time so the
    // indicator state machine knows we're not stale. has_ever_seen_wash_
    // gates the Searching -> FreshlyLocked transition (first lock gets
    // the solid-green grace period).
    const uint32_t now = now_ms();
    last_wash_received_ms_ = now;
    if (!has_ever_seen_wash_ || indicator_state_ == IndicatorState::Searching) {
        has_ever_seen_wash_ = true;
        lock_acquired_ms_   = now;
        indicator_state_    = IndicatorState::FreshlyLocked;
    }

    // Re-stamp on every wash event - a supersede resets the drift phase
    // so the new (r1,r2,cycle) takes immediate effect rather than
    // continuing the prior cycle's phase angle. Mirrors LocalDisplayBinding's
    // on_light_wash behaviour.
    wash_.active                = true;
    wash_.r1                    = ev.r1;
    wash_.g1                    = ev.g1;
    wash_.b1                    = ev.b1;
    wash_.r2                    = ev.r2;
    wash_.g2                    = ev.g2;
    wash_.b2                    = ev.b2;
    wash_.intensity             = ev.intensity;
    wash_.attack_100ms          = ev.attack;
    wash_.release_100ms         = ev.release;
    wash_.cycle_ms              = ev.cycle_ms;
    wash_.ttl_seconds           = ev.ttl_seconds;
    wash_.started_ms            = now;
    wash_.releasing_started_ms  = 0;
    return true;
}

bool LedStripDriver::send_wash_end(uint8_t /*target_group*/, uint8_t release_time) {
    if (!wash_.active) return false;
    // release_time overrides the wash's default release window. If both
    // are zero, snap to black on the next tick.
    if (release_time != 0) wash_.release_100ms = release_time;
    wash_.releasing_started_ms = now_ms();
    return true;
}

bool LedStripDriver::send_wash_pulse(uint8_t target_group, const RgbPulseEvent& ev) {
    // Sparkle-on-wash composes the same way as a plain pulse: pick a
    // random pixel, overlay, fade back to wash. The wash baseline is
    // already what every other pixel is showing, so a "kick + tail"
    // semantic falls out of the per-frame blend naturally.
    return send(target_group, ev);
}

// =============================================================================
// Sparkle (RgbPulseEvent)
// =============================================================================

bool LedStripDriver::send(uint8_t /*group_id*/, const RgbPulseEvent& ev) {
    if (!enabled() || !active_strip()) return false;

    // CHANCE filter is deferred to Phase 2. PixMob bracelets honour the
    // chance field at protocol level (the StickC PixMob driver also
    // pre-filters on the Director side); for the LED strip every
    // received sparkle renders. Operator-visible stochasticity will
    // come from Phase 2 per-pixel effects.

    const uint32_t duration_ms = pixmob_time_to_ms(static_cast<uint8_t>(ev.attack))
                               + pixmob_time_to_ms(static_cast<uint8_t>(ev.sustain))
                               + pixmob_time_to_ms(static_cast<uint8_t>(ev.release));
    spawn_sparkle(ev.r, ev.g, ev.b,
                   duration_ms == 0 ? 192u : duration_ms,
                   now_ms());
    return true;
}

size_t LedStripDriver::spawn_sparkle(uint8_t r, uint8_t g, uint8_t b,
                                      uint32_t duration_ms, uint32_t now) {
    auto* strip = active_strip();
    if (!strip) return kMaxSparkles;
    const size_t pcount = strip->pixel_count();
    if (pcount == 0) return kMaxSparkles;

    // Find a free slot, or evict the oldest if all are in flight.
    size_t free_slot   = kMaxSparkles;
    size_t oldest_slot = 0;
    uint32_t oldest_started = 0xFFFFFFFFu;
    for (size_t i = 0; i < kMaxSparkles; ++i) {
        if (!sparkles_[i].active) { free_slot = i; break; }
        if (sparkles_[i].started_ms < oldest_started) {
            oldest_started = sparkles_[i].started_ms;
            oldest_slot    = i;
        }
    }
    const size_t slot = (free_slot != kMaxSparkles) ? free_slot : oldest_slot;

    sparkles_[slot] = Sparkle{
        /*active=*/      true,
        /*pixel=*/       next_random() % pcount,
        /*r=*/           r,
        /*g=*/           g,
        /*b=*/           b,
        /*started_ms=*/  now,
        /*duration_ms=*/ duration_ms,
    };
    return slot;
}

// =============================================================================
// Per-tick render
// =============================================================================

bool LedStripDriver::compute_wash_baseline(uint32_t now,
                                            uint8_t& out_r,
                                            uint8_t& out_g,
                                            uint8_t& out_b) {
    if (!wash_.active) {
        out_r = out_g = out_b = 0;
        return false;
    }

    // Drift / hold colour.
    float r = static_cast<float>(wash_.r1);
    float g = static_cast<float>(wash_.g1);
    float b = static_cast<float>(wash_.b1);
    if (wash_.cycle_ms != 0) {
        const float phase = 2.0f * static_cast<float>(M_PI)
                          * static_cast<float>(now - wash_.started_ms)
                          / static_cast<float>(wash_.cycle_ms);
        const float t = 0.5f - 0.5f * std::cos(phase);
        r = static_cast<float>(wash_.r1) + (static_cast<float>(wash_.r2) - static_cast<float>(wash_.r1)) * t;
        g = static_cast<float>(wash_.g1) + (static_cast<float>(wash_.g2) - static_cast<float>(wash_.g1)) * t;
        b = static_cast<float>(wash_.b1) + (static_cast<float>(wash_.b2) - static_cast<float>(wash_.b1)) * t;
    }

    // Attack ramp.
    const uint32_t attack_ms = units100_to_ms(wash_.attack_100ms);
    if (attack_ms > 0 && wash_.releasing_started_ms == 0) {
        const uint32_t since = now - wash_.started_ms;
        if (since < attack_ms) {
            const float t = static_cast<float>(since) / static_cast<float>(attack_ms);
            r *= t; g *= t; b *= t;
        }
    }

    // TTL expiry - transition into release. Skips when ttl_seconds == 0.
    if (wash_.releasing_started_ms == 0 && wash_.ttl_seconds != 0) {
        const uint32_t ttl_ms = static_cast<uint32_t>(wash_.ttl_seconds) * 1000u;
        if (now - wash_.started_ms >= ttl_ms) {
            wash_.releasing_started_ms = now;
        }
    }

    // Release fade-out.
    if (wash_.releasing_started_ms != 0) {
        const uint32_t release_ms = units100_to_ms(wash_.release_100ms);
        if (release_ms == 0) {
            wash_.active = false;
            out_r = out_g = out_b = 0;
            return false;
        }
        const uint32_t since = now - wash_.releasing_started_ms;
        if (since >= release_ms) {
            wash_.active = false;
            out_r = out_g = out_b = 0;
            return false;
        }
        const float fade = 1.0f - (static_cast<float>(since) / static_cast<float>(release_ms));
        r *= fade; g *= fade; b *= fade;
    }

    // Intensity scalar.
    const float scale = static_cast<float>(wash_.intensity) / 255.0f;
    out_r = clip255(static_cast<int>(r * scale));
    out_g = clip255(static_cast<int>(g * scale));
    out_b = clip255(static_cast<int>(b * scale));
    return true;
}

void LedStripDriver::render_frame() {
    auto* strip = active_strip();
    if (!strip) return;
    const size_t pcount = strip->pixel_count();
    if (pcount == 0) return;

    const uint32_t now = now_ms();

    uint8_t base_r = 0, base_g = 0, base_b = 0;
    compute_wash_baseline(now, base_r, base_g, base_b);

    // Paint baseline. Even with no wash active, we still walk every
    // pixel - the indicator overlay may want pixel 0 lit on an
    // otherwise-black strip.
    for (size_t i = 0; i < pcount; ++i) {
        strip->set_pixel(i, base_r, base_g, base_b);
    }

    // Sparkle overlay. Each active sparkle linearly blends its peak
    // colour back to the baseline over duration_ms - bright at t=0,
    // baseline at t=duration. Multiple sparkles on the same pixel
    // overwrite (most-recent wins) which is fine in the 1-pixel-per
    // -spark model and rare in practice.
    for (size_t s = 0; s < kMaxSparkles; ++s) {
        Sparkle& sp = sparkles_[s];
        if (!sp.active) continue;
        const uint32_t since = now - sp.started_ms;
        if (sp.duration_ms == 0 || since >= sp.duration_ms) {
            sp.active = false;
            continue;
        }
        const float fade = static_cast<float>(since) / static_cast<float>(sp.duration_ms);
        const uint8_t r = lerp_u8(sp.r, base_r, fade);
        const uint8_t g = lerp_u8(sp.g, base_g, fade);
        const uint8_t b = lerp_u8(sp.b, base_b, fade);
        if (sp.pixel < pcount) {
            strip->set_pixel(sp.pixel, r, g, b);
        }
    }

    // Signal-state indicator (Epic 12 B5). Runs unconditionally - the
    // helper itself gates on signal_indicator_enabled_ and is a no-op
    // when disabled. Painted AFTER wash + sparkles so pixel 0 reflects
    // the indicator overlay during Searching / FreshlyLocked states,
    // and the wash baseline during Active state.
    update_and_paint_indicator(now, strip);

    // Always push - even when wash + sparkles are inactive, the
    // indicator may have written pixel 0 (Searching: pulsing green)
    // and we want that visible. The PixMob driver's per-tick airtime
    // concern doesn't apply here - one SPI/RMT show() is ~30 us per
    // pixel and the host is otherwise idle.
    strip->show();
}

void LedStripDriver::update_and_paint_indicator(uint32_t now,
                                                 hal::LedStrip* strip) {
    if (!signal_indicator_enabled_) return;
    if (!strip || strip->pixel_count() == 0) return;

    // State transitions.
    if (has_ever_seen_wash_) {
        const uint32_t since_last_wash = now - last_wash_received_ms_;
        if (since_last_wash > kNoSignalThresholdMs) {
            // Stale - signal lost. Drop back to Searching; next wash
            // will re-establish FreshlyLocked.
            indicator_state_ = IndicatorState::Searching;
        } else if (indicator_state_ == IndicatorState::FreshlyLocked) {
            // Hold solid-green grace window, then promote to Active so
            // pixel 0 reverts to wash render.
            if ((now - lock_acquired_ms_) >= kFreshLockDurationMs) {
                indicator_state_ = IndicatorState::Active;
            }
        }
    }

    // Paint pixel 0 per state.
    switch (indicator_state_) {
        case IndicatorState::Searching: {
            // 1 Hz, 50 % duty pulse - on for the first half of each
            // period, off for the second.
            const uint32_t phase = now % kIndicatorFlashPeriodMs;
            const bool     lit   = phase < (kIndicatorFlashPeriodMs / 2);
            strip->set_pixel(0, 0, lit ? 96 : 0, 0);
            break;
        }
        case IndicatorState::FreshlyLocked:
            strip->set_pixel(0, 0, 160, 0);
            break;
        case IndicatorState::Active:
            // Pixel 0 already painted by the wash + sparkle pass;
            // leave it alone.
            break;
    }
}

void LedStripDriver::loop_tick() {
    if (!enabled()) return;
    // Render every tick - the wash drift advances continuously and
    // sparkle fades need per-tick recomputation. The cost is one
    // show() call (~30 us/pixel) which at 30 pixels = ~1 ms per
    // 20 ms tick = 5%. Acceptable.
    render_frame();
}

// =============================================================================
// Singleton
// =============================================================================

LedStripDriver* led_strip_driver_instance() {
    static LedStripDriver instance;
    return &instance;
}

}  // namespace dal
}  // namespace nocturnation
