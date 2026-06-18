// PixMobIRDriver - DAL driver for the "ir-pixmob" transport.
//
// Translates RgbPulseEvents into PixMob IR pulse trains via the
// pixmob::buildSingleColor encoder, then transmits them through the HAL's
// IR transmitter(s).
//
// The encoder runs once per command; the resulting pulse train is fanned
// out to whichever emitter(s) are enabled - the built-in LED
// (hal::HAL::ir_tx(), "internal") and/or an external unit
// (hal::HAL::ir_tx_ext(), "external", Plus2 GPIO 26). Both on = both fire
// back-to-back, to boost coverage. The encoder/transmit split is unchanged;
// only the output stage fans out.
//
// Registered automatically by DAL::begin() when hal::HAL has IRTx. The
// driver is the join key for any active device whose profile declares
// transport = "ir-pixmob" (e.g. PixMobX4Gen3_1).
//
// Wash family (Epic 11). PixMob bracelets cannot natively hold a wash, but
// honour `pixmob::T_3840_MS` sustain envelopes - so a periodic SingleColor
// refresh at ~3 s cadence produces a continuous wash on the bracelet. This
// driver maintains per-target_group wash state and fires the refresh from
// its loop_tick(). See lume-capabilities-design.md §10 for the normative
// encoding contract and the bench results that grounded these parameters.

#pragma once

#include "dal/dal.h"

namespace nocturnation {
namespace dal {

class PixMobIRDriver : public Driver {
public:
    const char* transport_name() const override { return "ir-pixmob"; }

    bool begin() override;

    // Walk the active wash table and fire refreshes for any group whose
    // next_refresh_ms has elapsed. Called from the main loop via
    // DAL::loop_tick().
    void loop_tick() override;

    bool send(uint8_t group_id, const RgbPulseEvent&) override;
    bool send(uint8_t group_id, const AssignDeviceGroupEvent&) override;

    // Wash family (Epic 11 B2). Not virtual overrides because the wash
    // family doesn't fit the base Driver::send pattern - they need to
    // mutate per-group state inside the driver and the cancel path
    // takes a release time, not an event struct.
    //
    // send_wash: store wash state for the group, fire an immediate
    //   SingleColor at the wash colour, schedule the next refresh.
    // send_wash_end: stop refreshing the group. If release_time > 0,
    //   fire one final SingleColor with the closest pixmob::Time bucket
    //   as release for a faded exit; otherwise the bracelet's currently-
    //   active envelope completes naturally (snap-off at sustain end).
    // send_wash_pulse: fire TwoColors(sparkle, current_wash_rgb) - the
    //   "kick + tail" composite. If no wash is active for the group,
    //   falls back to SingleColor(sparkle) so a wash_pulse on a non-
    //   washing target still produces something.
    bool send_wash      (uint8_t target_group, const LightWashEvent&);
    bool send_wash_end  (uint8_t target_group, uint8_t release_time);
    bool send_wash_pulse(uint8_t target_group, const RgbPulseEvent&);

    // Emitter selection. The encoded frame is fanned out to each enabled
    // emitter. Internal is the built-in IR LED (default on, preserving the
    // pre-existing single-emitter behaviour); external is an optional unit
    // on hal::HAL::ir_tx_ext() (default off). Loaded from NVS at boot and
    // updated live from the IR config menu.
    void set_internal_enabled(bool e) { internal_enabled_ = e; }
    bool internal_enabled() const { return internal_enabled_; }
    void set_external_enabled(bool e) { external_enabled_ = e; }
    bool external_enabled() const { return external_enabled_; }

    // Fan one encoded pulse train out to every enabled emitter. Each
    // emitter is null-checked, so an enabled-but-absent external sink is a
    // no-op rather than a crash.
    //
    // Public so bench / test utilities (e.g. TestMode::PixMobBench in
    // Epic 11 B-0.5) can drive raw pulse trains without going through
    // the DAL render_fx path. The DAL's send() overloads are still the
    // canonical entry point for show-time traffic.
    void transmit(const uint16_t* pulses_us, size_t count);

    // Bench-test seam: how many slots in the active-wash table, exposed
    // so unit tests can iterate without hard-coding the magic number.
    static constexpr size_t kWashSlots = 10;   // groups 0..9

    // Refresh cadence (locked by Epic 11 B-0.5 bench - 3000 ms is the
    // smallest "no visible gaps" interval against the 3840 ms documented
    // sustain). Public constexpr so tests can use the same value.
    static constexpr uint32_t kWashRefreshMs = 3000;

    // Inject a wall-clock source for tests. Default (nullptr) uses
    // millis() on Arduino builds and a static 0 on native test envs.
    // Tests register a stub that advances under their control.
    using NowMsFn = uint32_t (*)();
    void set_clock_source(NowMsFn fn) { clock_source_ = fn; }

    // Bench-test seam: read the per-group state. Returns nullptr for
    // invalid group_id. Read-only - tests use this to assert state
    // without poking into private internals.
    struct WashState {
        bool     active;
        uint8_t  r1, g1, b1;
        uint8_t  r2, g2, b2;
        uint16_t cycle_ms;
        // Wash attack in 100 ms units, copied from LightWashEvent. Used
        // by each periodic refresh as the SingleColor's attack envelope
        // so the bracelet morphs smoothly between successive refresh
        // snapshots instead of snapping (which produces a visible
        // "alternating colour" artifact on drift washes - bench
        // observation 2026-06-18). Quantised to the nearest pixmob::Time
        // bucket at refresh time.
        uint8_t  attack_100ms;
        uint32_t started_ms;
        uint32_t next_refresh_ms;
    };
    const WashState* wash_state(uint8_t group_id) const;

private:
    bool internal_enabled_ = true;
    bool external_enabled_ = false;

    // One slot per target_group (0..9). Group 0 = broadcast (every
    // bracelet responds); 1..9 = group filter. The bracelet's own
    // group-id check decides which restrictGroupId frames it acts on.
    WashState wash_slots_[kWashSlots] = {};

    NowMsFn clock_source_ = nullptr;

    // Helpers.
    uint32_t now_ms() const;
    void     fire_wash_refresh(uint8_t group_id, const WashState& state,
                                uint32_t now);
    void     compute_drift_rgb(const WashState& state, uint32_t now,
                                uint8_t& r, uint8_t& g, uint8_t& b) const;
    // Map a wire-side 100 ms-unit time value (LightWashEvent.attack /
    // .release, LIGHT_WASH_END.release_time) to the closest pixmob::Time
    // enum bucket so we can pass it into pixmob::buildSingleColor.
    uint8_t  quantize_100ms_to_pixmob_time(uint8_t units_100ms) const;
};

PixMobIRDriver* pixmob_ir_driver_instance();

}  // namespace dal
}  // namespace nocturnation
