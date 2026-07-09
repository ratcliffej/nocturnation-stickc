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

// Apply brightness scaling to a single 8-bit channel. Two fixes over
// the naive `static_cast<int>(value * mul)`:
//   1. Round-half-up via `+ 0.5f` so 0.5 becomes 1 rather than the
//      truncation default of 0. Recovers borderline values - matters
//      enormously at low brightness where most products fall in the
//      0.0-1.0 range.
//   2. Min-1 floor: a non-zero authored channel with non-zero
//      brightness produces at least 1 on the wire. Without this, an
//      authored "subtle dim grey" like RGB(2, 5, 10) at brightness
//      10% renders as pure black on the strip (the operator-visible
//      colour goes missing). The proper fix for low-brightness
//      fidelity is temporal / spatial dithering (drives sub-8-bit
//      effective resolution by varying outputs across frames /
//      pixels); the min-1 floor is the simplest stopgap that
//      preserves *which channels are non-zero* even when the
//      magnitude rounds to 0.
inline uint8_t scale_channel(uint8_t value, float bri_mul) {
    if (value == 0 || bri_mul <= 0.0f) return 0;
    const int scaled = static_cast<int>(value * bri_mul + 0.5f);
    if (scaled <= 0) return 1;     // min-1 floor for visible authored colour
    return clip255(scaled);
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

void LedStripDriver::set_brightness_percent(uint8_t pct) {
    if (pct > 100) pct = 100;
    if (pct > max_brightness_pct_) pct = max_brightness_pct_;
#ifdef ARDUINO
    // Fleet-wide hard ceiling on production hardware. Belt-and-braces:
    // set_max_brightness_percent already clamps to kAbsoluteMaxBrightness
    // so this can only fire if a caller bypassed that path. Skipped on
    // native test envs so existing wash/sparkle tests can drive brightness
    // = 100 for literal RGB assertions.
    if (pct > kAbsoluteMaxBrightness) pct = kAbsoluteMaxBrightness;
#endif
    brightness_pct_ = pct;
}

void LedStripDriver::set_max_brightness_percent(uint8_t pct) {
    if (pct > 100) pct = 100;
#ifdef ARDUINO
    // Enforce the fleet-wide hard ceiling before recording. Any HAL
    // that returns > kAbsoluteMaxBrightness (mis-authored, legacy,
    // test) gets silently clamped so no downstream code path can see
    // a per-host cap that would allow rendering past the fleet
    // ceiling. See kAbsoluteMaxBrightness comment for rationale. Not
    // enforced under native test envs (bench code paths only).
    if (pct > kAbsoluteMaxBrightness) pct = kAbsoluteMaxBrightness;
#endif
    max_brightness_pct_ = pct;
    // Clamp the live brightness immediately so a max-cap reduction
    // takes effect at the next render, not "eventually".
    if (brightness_pct_ > max_brightness_pct_) brightness_pct_ = max_brightness_pct_;
}

void LedStripDriver::set_group_size(uint16_t n) {
    if (n == 0) n = 1;
    group_size_ = n;
}

void LedStripDriver::set_pixel_count(size_t n) {
    auto* strip = active_strip();
    if (!strip) return;
    strip->set_pixel_count(n);
    // Any in-flight per-pixel envelopes past the new count become
    // invisible (the render loop only walks up to pixel_count()) but
    // we clear them defensively so a future resize doesn't surface
    // a half-finished envelope on a pixel that wasn't being driven.
    // Relaxed store: set_pixel_count is called from the main loop
    // (Config menu / mode enter); no cross-core publish required.
    for (size_t i = n; i < kMaxPixels; ++i) {
        pixel_pulse_started_[i].store(0, std::memory_order_relaxed);
    }
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
    // continuing the prior cycle's phase angle.
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

    // Per-group CHANCE roll. Each group of group_size_ consecutive
    // pixels shares one roll - all pixels in that group light or all
    // stay dark per pulse. group_size_ = 1 means per-pixel rolls
    // (Phase 1 behaviour). A partial group at the strip's tail
    // shares a roll too.
    const uint8_t  chance_pct = chance_to_percent(static_cast<uint8_t>(ev.chance));
    const uint32_t now        = now_ms();
    const size_t   walk       = (pcount < kMaxPixels) ? pcount : kMaxPixels;
    const size_t   group      = (group_size_ == 0) ? 1u : group_size_;
    // Release-store publishes the current_pulse_ envelope + colour
    // writes above to any core observing this slot via acquire-load.
    // See the pixel_pulse_started_ member comment for the cross-core
    // race context. All stamps in this loop are release; render_frame's
    // per-pixel acquire load pairs with each individually.
    for (size_t i = 0; i < walk; i += group) {
        const bool hit = (next_random() % 100) < chance_pct;
        if (!hit) continue;
        const size_t end = (i + group < walk) ? (i + group) : walk;
        const uint32_t stamp = now ? now : 1;     // 0 reserved as inactive sentinel
        for (size_t p = i; p < end; ++p) {
            pixel_pulse_started_[p].store(stamp, std::memory_order_release);
        }
    }
    return true;
}

float LedStripDriver::pulse_envelope_fraction(uint32_t start_ms,
                                                uint32_t now,
                                                uint32_t attack_ms,
                                                uint32_t sustain_ms,
                                                uint32_t release_ms) {
    if (start_ms == 0) return 0.0f;
    const uint32_t elapsed = now - start_ms;
    const uint32_t total   = attack_ms + sustain_ms + release_ms;
    if (total == 0 || elapsed >= total) return 0.0f;
    // ASR shape: 0..a ramps in, a..a+s holds at 1.0, a+s..total ramps out.
    if (elapsed < attack_ms) {
        return attack_ms == 0
            ? 1.0f
            : static_cast<float>(elapsed) / static_cast<float>(attack_ms);
    }
    if (elapsed < attack_ms + sustain_ms) {
        return 1.0f;
    }
    return 1.0f
         - static_cast<float>(elapsed - attack_ms - sustain_ms)
         / static_cast<float>(release_ms);
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
    // Device brightness (0..100) scales the final colour BEFORE
    // set_pixel - so an operator-chosen dim setting brings the whole
    // wash + pulse render down. The signal indicator overlay is
    // applied after this loop and intentionally bypasses brightness
    // (system UI stays visible at any device setting).
    // Snapshot current_pulse_ ONCE per frame into locals. Prevents the
    // per-pixel loop from reading mixed generations of the shared struct
    // if a new send() lands on the WiFi task mid-loop. Same pattern as
    // Fable fleet-review finding #10 (PixMobIRDriver compute_drift_rgb
    // cycle_ms snapshot, stickc PR #25) - snapshot once, use locals from
    // there. Per-field 32-bit reads are individually atomic on Xtensa
    // (aligned uint32_t/uint8_t stores are single-instruction) so this
    // sequence of six reads can only tear if a send() write interleaves,
    // which the atomic acquire-load below rules out for any pixel whose
    // stamp we observe.
    const uint32_t pulse_a = current_pulse_.attack_ms;
    const uint32_t pulse_s = current_pulse_.sustain_ms;
    const uint32_t pulse_rt = current_pulse_.release_ms;
    const uint8_t  pulse_r = current_pulse_.r;
    const uint8_t  pulse_g = current_pulse_.g;
    const uint8_t  pulse_b = current_pulse_.b;
    const uint32_t pulse_total = pulse_a + pulse_s + pulse_rt;

    const size_t walk    = (pcount < kMaxPixels) ? pcount : kMaxPixels;
    const float  bri_mul = static_cast<float>(brightness_pct_) / 100.0f;
    for (size_t i = 0; i < walk; ++i) {
        uint8_t r = base_r, g = base_g, b = base_b;
        // Acquire-load pairs with the release-store in send(): any non-
        // zero stamp we observe implies the current_pulse_ envelope +
        // colour writes preceding the store are visible here. Combined
        // with the frame-start snapshot above, per-pixel envelope reads
        // are immune to a mid-frame send() overwriting the shared struct.
        const uint32_t stamp =
            pixel_pulse_started_[i].load(std::memory_order_acquire);
        if (stamp != 0 && pulse_total > 0) {
            const float alpha = pulse_envelope_fraction(
                stamp, now, pulse_a, pulse_s, pulse_rt);
            if (alpha <= 0.0f) {
                // Legitimately past envelope tail. Relaxed store OK -
                // clearing an inactive slot is idempotent; no consumer
                // depends on a happens-before edge here.
                pixel_pulse_started_[i].store(0,
                                                std::memory_order_relaxed);
            } else {
                r = lerp_u8(base_r, pulse_r, alpha);
                g = lerp_u8(base_g, pulse_g, alpha);
                b = lerp_u8(base_b, pulse_b, alpha);
            }
        }
        // Note: (stamp != 0 && pulse_total == 0) is the torn-envelope
        // defence-in-depth case - can't happen with the acquire above but
        // survives future refactors. We render base for this pixel and
        // leave the stamp; a later tick renders it properly.
        strip->set_pixel(i,
                          scale_channel(r, bri_mul),
                          scale_channel(g, bri_mul),
                          scale_channel(b, bri_mul));
    }
    // Tail of the buffer past kMaxPixels (if the strip is larger than
    // we can track envelopes for): paint baseline only, still scaled.
    for (size_t i = walk; i < pcount; ++i) {
        strip->set_pixel(i,
                          scale_channel(base_r, bri_mul),
                          scale_channel(base_g, bri_mul),
                          scale_channel(base_b, bri_mul));
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
