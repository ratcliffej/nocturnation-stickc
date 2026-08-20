// LedStripDriver - DAL driver for the "led-strip" transport (Epic 12).
//
// Translates wash-family + RgbPulse events into per-pixel writes against
// HAL::led_strip(). One driver per host - the strip is a single logical
// entity (onboard pixel + Grove pixels exposed as one contiguous array
// by the backend; see include/hal/hal.h class LedStrip).
//
// Visual contract (Phase 1):
//   - LIGHT_WASH paints every pixel with the wash baseline (drift +
//     attack/release/intensity per LightWashEvent). Whole strip renders
//     one colour at a time.
//   - LIGHT_PULSE / LIGHT_WASH_PULSE rolls the pulse's CHANCE field
//     independently per pixel. Each pixel that hits renders the pulse
//     envelope (ASR) over the wash baseline, then returns to baseline.
//     Treats each pixel as its own bracelet - identical semantics to
//     a PixMob bracelet swarm or the Tildagon perimeter ring, just
//     more pixels in the swarm.
//     -> CHANCE_100 effectively flashes the whole strip together.
//     -> CHANCE_16 lights ~17 % of pixels per pulse for a sparse twinkle.
//   - LIGHT_WASH_END fades all pixels to black over the release window.
//
// Phase 2 (per-pixel crawl / rainbow / gradient) is opt-in via richer
// event types not in this driver yet.
//
// Registered automatically by DAL::begin() when hal::HAL::led_strip()
// returns non-nullptr. On hosts without the capability the driver
// does not register and send() / loop_tick() calls return false.

#pragma once

#include <atomic>

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

    // Pulse dispatch. Rolls the event's CHANCE field independently per
    // pixel; hits start that pixel's envelope. Multiple pulses can
    // overlap - the latest pulse's colour + envelope governs any pixel
    // that gets re-triggered while still in a prior envelope.
    bool send(uint8_t group_id, const RgbPulseEvent&) override;

    // Wash family. Not driver-base overrides because the events take
    // a per-target_group argument that the strip doesn't use (single
    // physical strip per host - all groups land on the same strip).
    // group_id is accepted-but-ignored for API symmetry with
    // PixMobIRDriver / EspNowBroadcastDriver.
    bool send_wash      (uint8_t target_group, const LightWashEvent&);
    // v4 LED addressing (Epic 18): led_mode / led_modifier1 / led_modifier2
    // default to 0 (LedMode::All) so callers that pre-date the wire bump
    // continue to end the whole strip's wash. Mode-1 / mode-2 end
    // behaviour is deferred to the wash follow-up (per-pixel wash
    // state); for MVP the driver treats non-zero modes as end-whole-strip
    // and logs on native builds.
    bool send_wash_end  (uint8_t target_group, uint8_t release_time,
                          uint8_t led_mode = 0,
                          uint8_t led_modifier1 = 0,
                          uint8_t led_modifier2 = 0);
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

    // Inject a deterministic RNG for the per-pixel CHANCE roll. nullptr
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

    // Bench seams visible for test. kMaxPixels caps the per-pixel
    // envelope-state buffer. Operator can configure chain length up
    // to 288 pixels (2 m strip) via Config > LED Strip > Chain Size,
    // so the per-pixel envelope state needs that headroom too.
    static constexpr size_t kMaxPixels = 288;

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

    // -------------------------------------------------------------------------
    // Device brightness (Epic 12 B5 follow-on)
    // -------------------------------------------------------------------------
    //
    // Uniform 0..100 % multiplier applied to the wash + pulse render
    // path before each pixel hits the strip. Multiplies on top of any
    // per-event `intensity` field (LightWashEvent.intensity), so a
    // wash with intensity = 200 at device brightness = 25 % renders
    // at 200 * 0.25 = 50.
    //
    // Does NOT scale the LumeMode-driven pixel-0 overlay - that's
    // system UI and stays at fixed brightness regardless of device
    // setting. NVS-persisted via persistence::load_strip_brightness();
    // applied at mode entry through DAL::apply_persisted_strip_settings.
    // Config > Connectivity > LED Strip > Brightness cycles the
    // operator-visible values {5, 10, 25, 50} on Plus2 / S3; the 25 and
    // 50 tiers require external Grove Y power (M5Stack TypeC2Grove Unit
    // U151) so the strip's V-rail comes off USB-C rather than the
    // device's regulator. Atom Lite has no menu and is HAL-capped at 10.
    void set_brightness_percent(uint8_t pct);
    uint8_t brightness_percent() const { return brightness_pct_; }

    // Fleet-wide HARD ceiling on strip brightness. Enforced on ARDUINO
    // builds only (production hardware); native test envs bypass so
    // existing wash / sparkle assertions can drive brightness = 100.
    // On production, applies to every host regardless of HAL's
    // max_strip_brightness_percent(); no code path (persistence load,
    // Btn1 cycle, Config menu, direct API) can raise the rendered
    // brightness above this value.
    //
    // Raised 10 -> 50 on 2026-07-11 once the M5Stack TypeC2Grove Unit
    // (U151) landed - that Y adapter takes USB-C 5V straight to the
    // strip's V pin, bypassing the device's regulator, so the 10 %
    // brownout ceiling on internal-battery / device-USB power no
    // longer bounds the strip. Per-host HAL caps still enforce the
    // per-device envelope: Plus2 + S3 raise to 50 (matches this new
    // ceiling), Atom Lite stays at 10 (no Config menu to reach
    // higher values, safest default for standalone deployments).
    //
    // The 25 and 50 tiers on the Config menu are labelled "(Ext Grove
    // Pwr)" because they require the Y adapter; internal-battery /
    // direct-USB-to-device power will brown out at those levels on
    // a 29-pixel SK6812 chain. Bench evidence 2026-07-08 to 2026-07-11.
    // Change this number only in coordination with a per-host HAL cap
    // change AND a documented external-power expectation.
    static constexpr uint8_t kAbsoluteMaxBrightness = 50;

    // Per-host maximum brightness cap. set_brightness_percent above
    // clamps to this; set via DAL::apply_persisted_strip_settings()
    // from the HAL's max_strip_brightness_percent() at mode entry.
    // Additionally clamped to kAbsoluteMaxBrightness so a mis-authored
    // HAL returning e.g. 100 can't bypass the fleet-wide ceiling.
    void set_max_brightness_percent(uint8_t pct);
    uint8_t max_brightness_percent() const { return max_brightness_pct_; }

    // -------------------------------------------------------------------------
    // Group size (Epic 12 follow-on, Config > LED Strip > Group Size)
    // -------------------------------------------------------------------------
    //
    // CHANCE rolls run per group of N pixels instead of per pixel.
    // group_size = 1  -> every pixel rolls its own die (matches the
    //                    Tildagon perimeter ring's per-LED sparkle).
    // group_size = 12 -> 12 pixels share a roll and flash together
    //                    (Tildagon-ring-sized block on a long strip).
    // group_size = pixel_count -> whole strip rolls once (matches the
    //                    PixMob bracelet's whole-device response to
    //                    a single CHANCE roll).
    // A partial group at the strip's tail rolls together too.
    //
    // Only affects LIGHT_PULSE / LIGHT_WASH_PULSE rolls; wash baseline
    // is uniform across all pixels regardless of group size.
    void set_group_size(uint16_t n);
    uint16_t group_size() const { return group_size_; }

    // Resize the underlying HAL strip. Forwards to HAL::LedStrip::
    // set_pixel_count which (for Adafruit_NeoPixel-backed backends)
    // reallocates the buffer so show() only walks the configured count.
    void set_pixel_count(size_t n);

private:
    // Most-recent pulse colour + envelope, shared across all pixels
    // currently rendering this pulse's envelope. A new pulse arriving
    // mid-envelope supersedes - any pixel re-triggered in the new
    // pulse's CHANCE roll starts from this pulse's colour. Pixels
    // still mid-envelope from the prior pulse will visually drift to
    // the new colour as they re-trigger (or finish their envelope and
    // return to baseline).
    struct Pulse {
        uint8_t  r, g, b;
        uint32_t attack_ms;
        uint32_t sustain_ms;
        uint32_t release_ms;
    };

    WashState wash_              = {};
    Pulse     current_pulse_     = {};
    // Per-pixel envelope start time. 0 = pixel is inactive (no pulse
    // currently rendering on it). On a pulse arrival, the CHANCE roll
    // sets started_ms = now for hits; render_frame walks each pixel
    // and clears the stamp when the envelope elapses.
    //
    // Atomic to close a cross-core torn-read race on ESP32: send() runs
    // on the WiFi task (LumeLedStripBinding declares can_render_in_
    // callback = true) while render_frame() runs on the main loop. A
    // release-store to this slot in send() publishes the accompanying
    // current_pulse_ writes; the acquire-load in render_frame() sees
    // them coherently. Before this, a bare `uint32_t` array let render
    // observe the new stamp before the envelope writes had propagated -
    // total==0 read → alpha==0 → pixel cleared → whole pulse silently
    // dropped. Symptom was random ~1-in-5 pulse drops with no colour
    // pattern, whole strip dark for that frame. Diagnosed 2026-07-08.
    std::atomic<uint32_t> pixel_pulse_started_[kMaxPixels] = {};

    NowMsFn        clock_source_   = nullptr;
    hal::LedStrip* strip_override_ = nullptr;
    RandFn         rng_source_     = nullptr;
    uint32_t       rng_state_      = 0;

    // Pixel 0 overlay state. Owned by LumeMode via set_overlay_pixel_0.
    bool    overlay_enabled_ = false;
    uint8_t overlay_r_       = 0;
    uint8_t overlay_g_       = 0;
    uint8_t overlay_b_       = 0;

    // Device brightness. 0..100; defaults to 100 so a freshly-constructed
    // driver matches pre-brightness-feature behaviour. LumeMode loads
    // the persisted value via persistence::load_strip_brightness() and
    // applies it on enter().
    uint8_t brightness_pct_     = 100;
    uint8_t max_brightness_pct_ = 100;   // host caps to this; see set_max_brightness_percent

    // Group size for CHANCE rolls. 1 = per-pixel (original Phase 1).
    uint16_t group_size_ = 1;

    // Active strip - the override (test) or HAL::led_strip() (production).
    hal::LedStrip* active_strip() const;
    uint32_t       now_ms() const;
    uint32_t       next_random();

    // Per-tick render path. Walks every pixel - computes the wash
    // baseline, then if that pixel is mid-envelope blends in the
    // current pulse colour by ASR position. Overlay pixel 0 last.
    void render_frame();

    // Compute the wash baseline RGB for the current tick, including
    // drift ping-pong + attack/release ramps. Returns true if there's
    // anything to render (false skips the show() call to avoid
    // wasting SPI/RMT bandwidth when nothing changes - e.g. a long
    // static hold after the attack ramp finished).
    bool compute_wash_baseline(uint32_t now,
                                 uint8_t& out_r, uint8_t& out_g, uint8_t& out_b);

    // Compute the per-pixel envelope multiplier (0..1) for a pulse that
    // started at start_ms and is now at `now`. Takes the ASR fields
    // explicitly (rather than reading current_pulse_ inside) so the
    // caller can pass a per-render snapshot - see render_frame() for
    // the cross-task tearing argument. Returns 0 when the pulse has
    // elapsed (caller treats as "envelope done, clear the slot").
    static float pulse_envelope_fraction(uint32_t start_ms, uint32_t now,
                                          uint32_t attack_ms,
                                          uint32_t sustain_ms,
                                          uint32_t release_ms);
};

LedStripDriver* led_strip_driver_instance();

}  // namespace dal
}  // namespace nocturnation
