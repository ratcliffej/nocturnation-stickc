// LedStripDriver - DAL driver for the "led-strip" transport (Epic 12).
//
// Translates wash-family + RgbPulse events into per-pixel writes against
// HAL::led_strip(). One driver per host - the strip is a single logical
// entity (onboard pixel + Grove pixels exposed as one contiguous array
// by the backend; see include/hal/hal.h class LedStrip).
//
// Wash semantic (Phase 1):
//   - Whole strip renders one colour at a time.
//   - LIGHT_WASH with cycle_ms == 0 holds (r1,g1,b1) on every pixel.
//   - LIGHT_WASH with cycle_ms > 0 cosine-eased ping-pong between
//     (r1,g1,b1) and (r2,g2,b2) over cycle_ms - identical to the
//     PixMob IR driver's drift envelope so cross-fleet cues land
//     in colour-sync (within IR airtime jitter).
//   - LIGHT_WASH_END fades to black over the release window.
//   - LIGHT_WASH_PULSE / LIGHT_PULSE picks one random pixel and
//     overlays the pulse colour for the sparkle duration, then
//     blends back to wash.
//
// Phase 2 (per-pixel crawl / rainbow / gradient) is opt-in via richer
// event types not in this driver yet.
//
// Registered automatically by DAL::begin() when hal::HAL::led_strip()
// returns non-nullptr. On hosts without the capability the driver
// does not register and send() / loop_tick() calls return false.

#pragma once

#include "dal/dal.h"
#include "hal/hal.h"

namespace nocturnation {
namespace dal {

class LedStripDriver : public Driver {
public:
    const char* transport_name() const override { return "led-strip"; }

    // Returns false (skips registration) when hal::HAL::led_strip() is
    // nullptr on this backend. On a valid backend, initialises the strip
    // (begin + clear + show one black frame so the host doesn't boot
    // with the LEDs at whatever state the silicon powered up in).
    bool begin() override;

    // Renders the current wash state to every pixel at the per-tick
    // cadence (driven by DAL::loop_tick()). Cheap when inactive; runs
    // the drift / attack / release maths and one strip->show() call
    // when active.
    void loop_tick() override;

    // Sparkle dispatch. Picks one random pixel, overlays the pulse
    // colour, blends back to the wash baseline over the event's
    // attack + sustain + release window. Multiple sparkles in flight
    // are tracked up to kMaxSparkles; new sparkles past the cap
    // replace the oldest still-in-flight slot.
    bool send(uint8_t group_id, const RgbPulseEvent&) override;

    // Wash family. Not driver-base overrides because the events take
    // a per-target_group argument that the strip doesn't use (single
    // physical strip per host - all groups land on the same strip).
    // group_id is accepted-but-ignored for API symmetry with
    // PixMobIRDriver / EspNowBroadcastDriver.
    bool send_wash      (uint8_t target_group, const LightWashEvent&);
    bool send_wash_end  (uint8_t target_group, uint8_t release_time);
    bool send_wash_pulse(uint8_t target_group, const RgbPulseEvent&);

    // -------------------------------------------------------------------------
    // Bench / test seams
    // -------------------------------------------------------------------------

    // Inject a wall-clock source for tests. nullptr (default) uses
    // millis() on Arduino builds and a static 0 on native test envs.
    using NowMsFn = uint32_t (*)();
    void set_clock_source(NowMsFn fn) { clock_source_ = fn; }

    // Inject a strip override for native tests. nullptr (default) uses
    // hal::HAL::led_strip(). A recording mock implementation in the
    // test TU lets the suite assert on actual pixel writes.
    void set_strip_override(hal::LedStrip* strip) { strip_override_ = strip; }

    // Inject a deterministic RNG for sparkle pixel selection. nullptr
    // (default) uses a tiny xorshift seeded from millis() at first call.
    // Tests register a stub that returns a known sequence.
    using RandFn = uint32_t (*)();
    void set_rng_source(RandFn fn) { rng_source_ = fn; }

    // Inspect internal wash state. Returns nullptr if wash inactive.
    struct WashState {
        bool     active;
        uint8_t  r1, g1, b1;
        uint8_t  r2, g2, b2;
        uint8_t  intensity;
        uint8_t  attack_100ms;
        uint8_t  release_100ms;
        uint16_t cycle_ms;
        uint16_t ttl_seconds;
        uint32_t started_ms;
        uint32_t releasing_started_ms;     // 0 = not in release phase
    };
    const WashState* wash_state() const {
        return wash_.active ? &wash_ : nullptr;
    }

    // Bench seams visible for test.
    static constexpr size_t kMaxSparkles = 4;

    // -------------------------------------------------------------------------
    // Pixel 0 overlay (Epic 12 B5)
    // -------------------------------------------------------------------------
    //
    // LumeMode owns the signal-state policy (when to flash, when to
    // hold solid, when to yield) - this driver is the mechanism layer
    // that simply overrides pixel 0 with whatever colour LumeMode
    // hands it on each tick. When `enabled` is false the driver
    // renders pixel 0 as part of the normal wash/sparkle pass.
    //
    // The decoupling reflects the architecture's DRY principle: on
    // hosts with a Display, LumeMode draws the NO SIGNAL pip via
    // fire_display_*; on hosts with only LedStrip (Atom Lite), it
    // drives this overlay. Same Lume logic, different output sink -
    // the variance lives in HAL capability queries, not in
    // duplicated state machines.
    void set_overlay_pixel_0(uint8_t r, uint8_t g, uint8_t b, bool enabled);

private:
    struct Sparkle {
        bool     active;
        size_t   pixel;
        uint8_t  r, g, b;
        uint32_t started_ms;
        uint32_t duration_ms;
    };

    WashState wash_   = {};
    Sparkle   sparkles_[kMaxSparkles] = {};

    NowMsFn        clock_source_   = nullptr;
    hal::LedStrip* strip_override_ = nullptr;
    RandFn         rng_source_     = nullptr;
    uint32_t       rng_state_      = 0;

    // Pixel 0 overlay state. Owned by LumeMode via set_overlay_pixel_0.
    bool    overlay_enabled_ = false;
    uint8_t overlay_r_       = 0;
    uint8_t overlay_g_       = 0;
    uint8_t overlay_b_       = 0;

    // Active strip - the override (test) or HAL::led_strip() (production).
    hal::LedStrip* active_strip() const;
    uint32_t       now_ms() const;
    uint32_t       next_random();

    // Per-tick render path. Computes the wash baseline RGB, paints
    // every pixel with it (applying intensity), then overlays any
    // active sparkles, then calls strip->show().
    void render_frame();

    // Compute the wash baseline RGB for the current tick, including
    // drift ping-pong + attack/release ramps. Returns true if there's
    // anything to render (false skips the show() call to avoid
    // wasting SPI/RMT bandwidth when nothing changes - e.g. a long
    // static hold after the attack ramp finished).
    bool compute_wash_baseline(uint32_t now,
                                 uint8_t& out_r, uint8_t& out_g, uint8_t& out_b);

    // Spawn a sparkle on a random pixel. Returns the slot index used
    // (or kMaxSparkles when the strip has no pixels to spark on).
    size_t spawn_sparkle(uint8_t r, uint8_t g, uint8_t b,
                          uint32_t duration_ms, uint32_t now);
};

LedStripDriver* led_strip_driver_instance();

}  // namespace dal
}  // namespace nocturnation
