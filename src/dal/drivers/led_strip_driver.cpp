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
    return true;
}

void LedStripDriver::set_overlay_pixel_0(uint8_t r, uint8_t g, uint8_t b,
                                          bool enabled) {
    overlay_enabled_ = enabled;
    overlay_r_       = r;
    overlay_g_       = g;
    overlay_b_       = b;
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
    wash_.started_ms            = now_ms();
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
    // LIGHT_WASH_PULSE composes the same way as a plain LIGHT_PULSE:
    // each pixel rolls CHANCE independently, hits render the pulse
    // envelope blended over the wash baseline. No special path.
    return send(target_group, ev);
}

// =============================================================================
// Pulse (RgbPulseEvent) - per-pixel CHANCE roll
// =============================================================================

namespace {
// Map a pixmob::Chance enum bucket onto a 0..100 percent. The enum
// bits aren't a linear scale (it's a discrete LUT in the protocol
// docs), so this is a flat switch.
inline uint8_t chance_to_percent(uint8_t bucket) {
    switch (bucket) {
        case 0: return 100;     // CHANCE_100
        case 1: return 88;      // CHANCE_88
        case 2: return 67;      // CHANCE_67
        case 3: return 50;      // CHANCE_50
        case 4: return 32;      // CHANCE_32
        case 5: return 16;      // CHANCE_16
        case 6: return 10;      // CHANCE_10
        case 7: return 4;       // CHANCE_4
        default: return 100;
    }
}
}  // namespace

bool LedStripDriver::send(uint8_t /*group_id*/, const RgbPulseEvent& ev) {
    if (!enabled()) return false;
    auto* strip = active_strip();
    if (!strip) return false;

    const size_t pcount = strip->pixel_count();
    if (pcount == 0) return true;

    // Latch this pulse as "the current pulse" - colour + envelope
    // shared across all pixels that hit on this roll. Any pixel still
    // mid-envelope from a prior pulse will get re-stamped if its
    // chance roll hits again (visually: smooth crossfade to the new
    // colour); otherwise it finishes its prior envelope naturally.
    current_pulse_.r          = ev.r;
    current_pulse_.g          = ev.g;
    current_pulse_.b          = ev.b;
    current_pulse_.attack_ms  = pixmob_time_to_ms(static_cast<uint8_t>(ev.attack));
    current_pulse_.sustain_ms = pixmob_time_to_ms(static_cast<uint8_t>(ev.sustain));
    current_pulse_.release_ms = pixmob_time_to_ms(static_cast<uint8_t>(ev.release));
    // Guard against a zero-length envelope (T_0 attack + T_0 sustain +
    // T_0 release) - that would never render. Floor at 192 ms (one
    // "natural" sparkle duration via T_192 sustain).
    if (current_pulse_.attack_ms + current_pulse_.sustain_ms +
        current_pulse_.release_ms == 0) {
        current_pulse_.sustain_ms = 192;
    }

    // Per-pixel CHANCE roll. Each pixel is its own "bracelet" - a
    // pulse with CHANCE_50 hits ~half the pixels; CHANCE_16 hits
    // ~17 %. CHANCE_100 hits every pixel and effectively flashes
    // the whole strip together.
    const uint8_t chance_pct = chance_to_percent(static_cast<uint8_t>(ev.chance));
    const uint32_t now       = now_ms();
    const size_t   walk      = (pcount < kMaxPixels) ? pcount : kMaxPixels;
    for (size_t i = 0; i < walk; ++i) {
        if ((next_random() % 100) < chance_pct) {
            pixel_pulse_started_[i] = now ? now : 1;   // 0 reserved as inactive sentinel
        }
    }
    return true;
}

float LedStripDriver::pulse_envelope_fraction(uint32_t start_ms,
                                                uint32_t now) const {
    if (start_ms == 0) return 0.0f;
    const uint32_t elapsed = now - start_ms;
    const uint32_t a = current_pulse_.attack_ms;
    const uint32_t s = current_pulse_.sustain_ms;
    const uint32_t r = current_pulse_.release_ms;
    const uint32_t total = a + s + r;
    if (total == 0 || elapsed >= total) return 0.0f;
    // ASR shape: 0..a ramps in, a..a+s holds at 1.0, a+s..total ramps out.
    if (elapsed < a) {
        return a == 0 ? 1.0f : static_cast<float>(elapsed) / static_cast<float>(a);
    }
    if (elapsed < a + s) {
        return 1.0f;
    }
    return 1.0f - static_cast<float>(elapsed - a - s) / static_cast<float>(r);
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

    // Walk every pixel. Each pixel is its own bracelet: paint the
    // wash baseline first, then if this pixel is mid-pulse-envelope
    // blend toward the pulse colour by the ASR position. A pixel
    // with pixel_pulse_started_ == 0 just shows the baseline.
    const size_t walk = (pcount < kMaxPixels) ? pcount : kMaxPixels;
    for (size_t i = 0; i < walk; ++i) {
        uint8_t r = base_r, g = base_g, b = base_b;
        if (pixel_pulse_started_[i] != 0) {
            const float alpha = pulse_envelope_fraction(
                pixel_pulse_started_[i], now);
            if (alpha <= 0.0f) {
                pixel_pulse_started_[i] = 0;
            } else {
                // alpha = 0 -> baseline, alpha = 1 -> pulse peak.
                // Linear blend baseline -> pulse colour. The
                // baseline is whatever the wash gives us (often
                // black for pulse-only shows).
                r = lerp_u8(base_r, current_pulse_.r, alpha);
                g = lerp_u8(base_g, current_pulse_.g, alpha);
                b = lerp_u8(base_b, current_pulse_.b, alpha);
            }
        }
        strip->set_pixel(i, r, g, b);
    }
    // Tail of the buffer past kMaxPixels (if the strip is larger than
    // we can track envelopes for): paint baseline only.
    for (size_t i = walk; i < pcount; ++i) {
        strip->set_pixel(i, base_r, base_g, base_b);
    }

    // Pixel 0 overlay. LumeMode (or any other policy layer) sets the
    // overlay colour + enable flag via set_overlay_pixel_0; the driver
    // simply paints it after wash + sparkles so it always wins. When
    // disabled, pixel 0 belongs to the wash render like every other
    // pixel.
    if (overlay_enabled_ && pcount > 0) {
        strip->set_pixel(0, overlay_r_, overlay_g_, overlay_b_);
    }

    strip->show();
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
