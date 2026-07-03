#include "pixmob_ir_driver.h"
#include "hal/hal.h"
#include "pixmob_protocol.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace dal {

namespace {
PixMobIRDriver s_instance;

// Working buffer for the pulse train. 80 entries is plenty for any 9-byte
// PixMob command (the longest transmission is well under that). Sized to
// match the prototype's irBuf so behaviour is byte-identical.
constexpr size_t kPulseBufSize = 80;
}  // namespace

PixMobIRDriver* pixmob_ir_driver_instance() { return &s_instance; }

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool PixMobIRDriver::begin() {
    // Refuse registration if there is no IRTx to drive.
    return hal::HAL::ir_tx() != nullptr;
}

// -----------------------------------------------------------------------------
// Emitter fan-out
// -----------------------------------------------------------------------------

void PixMobIRDriver::transmit(const uint16_t* pulses_us, size_t count) {
    // 38 kHz carrier per the PixMob protocol. Internal then external, fired
    // back-to-back; IRsend::sendRaw blocks per burst, so "both on" emits two
    // sequential bursts a few ms apart - invisible against PixMob's tens-of-ms
    // envelopes, and it doubles coverage.
    if (internal_enabled_) {
        auto* ir = hal::HAL::ir_tx();
        if (ir) ir->send_raw(pulses_us, count, 38);
    }
    if (external_enabled_) {
        auto* ir = hal::HAL::ir_tx_ext();
        if (ir) ir->send_raw(pulses_us, count, 38);
    }
}

// -----------------------------------------------------------------------------
// Output dispatch
// -----------------------------------------------------------------------------

bool PixMobIRDriver::send(uint8_t group_id, const RgbPulseEvent& ev) {
    // Epic 11 bench (2026-06-18): pulses on a washing group are a
    // protocol headache on this bracelet generation. TwoColors (type
    // 0b010) - the documented "flash colour 1 briefly, then hold
    // colour 2" composite that maps 1:1 to "sparkle + return to wash"
    // - renders as nothing visible on this firmware revision
    // (PMob Bench T6 confirmed; isolated buildTwoColors(red, blue)
    // produced no light on the bracelet at all). Earlier TwoColors-
    // based fix (commit f1b4e39) was therefore a silent no-op: the
    // wash held smoothly but the sparkles never landed visibly.
    //
    // SingleColor (type 0b000) does render, but each new SingleColor
    // wholesale replaces the bracelet's current envelope - so a sparkle
    // ends with the bracelet in release going to black, with no
    // protocol-level "return to wash" mechanism. Fix: compose the
    // semantic from two SingleColors:
    //   1. SingleColor(sparkle_rgb, orchestrator envelope) - the
    //      visible flash. Uses whatever envelope the show layer asked
    //      for; the bracelet snaps to sparkle and starts that envelope.
    //   2. SingleColor(current_wash_rgb, fast attack) - the recovery,
    //      fired back-to-back. The bracelet pre-empts the sparkle
    //      mid-envelope and morphs from wherever it currently is to
    //      the wash colour over ~192 ms. Net visible effect: brief
    //      sparkle (the inter-command IR transmission window, ~50 ms)
    //      plus a quick morph back to the wash. The wash colour
    //      target uses the same lookahead-shifted phase the periodic
    //      refresh uses, so the bracelet ends the morph in sync with
    //      the Tildagon's continuous drift.
    //
    // The fixed 192 ms recovery attack is decoupled from the wash's
    // configured attack (which is for initial fade-in, not sparkle
    // recovery). At 120 BPM (500 ms between beats) the bracelet
    // spends ~250 ms on wash colour between sparkles - enough wash
    // continuity to read as "wash with accents" rather than "strobe
    // with dark gaps".
    if (group_id < kWashSlots && wash_slots_[group_id].active) {
        const uint32_t t = now_ms();

        // Chance pre-filter. Bench finding 2026-06-30 (revised): the
        // bracelet accumulates "stress" from processing IR commands
        // it ultimately skips - so even though CHANCE_16 means only
        // 16 % of received sparkles render, the bracelet still
        // decodes and processes the other 84 %, and after enough of
        // those skipped commands the bracelet's IR receiver enters a
        // degraded mode where it ignores most subsequent commands
        // (including periodic refreshes), causing the 3-second
        // blackout pattern. The degraded state persists across
        // StickC mode changes (running music then switching to Wash
        // Test still shows the blip; only a bracelet power-cycle
        // recovers).
        //
        // Fix: roll the chance on the StickC side instead of letting
        // the bracelet skip them. If the roll fails, return without
        // firing IR - the bracelet doesn't see anything. If the roll
        // passes, fire with CHANCE_100 (force the bracelet to
        // render). Total IR airtime drops by ~6x for typical music
        // (CHANCE_16 = 1 in 6 commands actually transmitted), giving
        // the bracelet's receiver headroom.
        //
        // Trade-off: bracelets sparkle in unison instead of with
        // per-bracelet randomness. For a small fleet (4 groups
        // expected for EMF deployment) this reads as "synchronised
        // crowd accent" rather than "random twinkle" - a different
        // aesthetic but not worse for music-driven cues.
        if (ev.chance != pixmob::CHANCE_100) {
#ifdef ARDUINO
            const uint32_t roll = static_cast<uint32_t>(::esp_random());
#else
            const uint32_t roll = static_cast<uint32_t>(std::rand());
#endif
            // pixmob::Chance bucket -> render probability (1/256ths
            // of 256). CHANCE_100 = 256/256, CHANCE_88 = 224/256,
            // CHANCE_67 = 170/256, CHANCE_50 = 128/256, CHANCE_32 =
            // 82/256, CHANCE_16 = 41/256, CHANCE_10 = 26/256,
            // CHANCE_4 = 10/256. Roll mod 256 then compare.
            static constexpr uint8_t kRenderThreshold[8] = {
                255, 224, 170, 128, 82, 41, 26, 10,
            };
            const uint8_t threshold = kRenderThreshold[ev.chance & 0x07];
            if ((roll & 0xFF) >= threshold) {
#if defined(ARDUINO) && !defined(NOCT_DMX_ETHERNET)   // off on the console board
                // Bench-diagnostic: tells the operator a sparkle was
                // skipped by the chance pre-filter rather than dropped
                // somewhere downstream. Helps tell apart "wash blocking
                // pulse" from "chance just didn't roll high enough".
                Serial.printf("[pixmob] PULSE skip chance=%u roll=%u thr=%u grp=%u\n",
                              (unsigned)ev.chance, (unsigned)(roll & 0xFF),
                              (unsigned)threshold, (unsigned)group_id);
#endif
                return true;   // skip this sparkle - don't fire IR
            }
        }

        // Compute the wash colour for the recovery.
        uint8_t wr = wash_slots_[group_id].r1;
        uint8_t wg = wash_slots_[group_id].g1;
        uint8_t wb = wash_slots_[group_id].b1;
        if (wash_slots_[group_id].cycle_ms > 0) {
            compute_drift_rgb(wash_slots_[group_id], t, wr, wg, wb);
        }

        // 1. The sparkle. Bench finding 2026-06-18: 3x redundancy
        // burst was tried and dropped - dropouts are fully correlated
        // to the bracelet's busy-rendering-attack state, so a burst
        // either all-lands or all-drops, and "all lands" produces
        // visually distracting duplicate flashes. Single command is
        // the cleaner shape. Hit rate depends on the refresh attack
        // having been changed to T_0_MS in fire_wash_refresh, which
        // keeps the bracelet's busy-window short.
        //
        // Bench finding 2026-06-30: with the orchestrator's
        // wash_with_sparkle FX (which writes pulse SUSTAIN = 16 raw
        // DMX, quantising to T_0_MS) the bracelet completes its
        // release tail to black and the recovery x2 below STILL
        // drops - the bracelet's refractory window during release-
        // rendering is longer than during sustain-rendering, so both
        // recoveries hit the bracelet while it's deaf. Symptom: 3-
        // second blackout pattern during music playback, matching
        // the rendered-sparkle rate (CHANCE_16 quantisation: 16 % of
        // sparkles render, 1.87 Hz beat * 0.16 ~ 0.3 Hz = 3.3 s).
        // Fix: clamp the sparkle's sustain to >= T_32_MS so the
        // bracelet's response has a brief sustain phase rather than
        // jumping straight to release. The 32 ms hold is below the
        // human flash-perception threshold so the visual difference
        // is sub-visible, but the bracelet's IR receiver stays
        // receptive enough during that 32 ms for the recovery x2 to
        // land. Wash Test's "Pulse over wash" already used T_32_MS
        // sustain explicitly and worked cleanly - this brings the
        // music path into parity.
        // Sparkle sustain minimum. T_32_MS is enough because the 50 ms
        // delay below provides the visible-sparkle window - the
        // bracelet renders the sparkle envelope for those 50 ms
        // before the recovery pre-empts.
        auto effective_sustain = ev.sustain;
        if (effective_sustain == pixmob::T_0_MS) {
            effective_sustain = pixmob::T_32_MS;
        }

        // Override sparkle release to T_0 too. User-bench hypothesis
        // 2026-06-30: the bracelet "remembers" recent envelope
        // timings and uses them to drive an internal cycle that
        // briefly idles the LED ~3 s after a long envelope. Sparkles
        // typically come in with T_192_MS release (from the
        // orchestrator's wash_with_sparkle FX, raw 96 DMX -> T_192);
        // forcing release to T_0_MS minimises the "envelope footprint"
        // the bracelet records.
        uint16_t buf[kPulseBufSize];
        size_t n = pixmob::buildSingleColor(
            buf, kPulseBufSize,
            ev.r, ev.g, ev.b,
            ev.attack, effective_sustain, pixmob::T_0_MS,
            pixmob::CHANCE_100,    // chance roll already done above; bracelet always renders
            group_id);
        if (n == 0) return false;
        transmit(buf, n);
#if defined(ARDUINO) && !defined(NOCT_DMX_ETHERNET)   // off on the console board
        // Bench-diagnostic: the sparkle command landed on the IR LED.
        Serial.printf("[pixmob] PULSE tx rgb=%02X%02X%02X grp=%u "
                      "wash=(%02X%02X%02X)\n",
                      (unsigned)ev.r, (unsigned)ev.g, (unsigned)ev.b,
                      (unsigned)group_id,
                      (unsigned)wr, (unsigned)wg, (unsigned)wb);
#endif

        // Inter-frame quiet period (regression fix 2026-06-23). The
        // bracelet's IR decoder needs a gap between commands to lock
        // onto each one cleanly - the PixMob protocol has no explicit
        // end-of-frame sentinel, so without a quiet window the
        // decoder mis-decodes back-to-back frames and drops both.
        // First verified on bench 2026-06-18 (commit 3469a7c); lost
        // when febc7f3 swapped a 3x sparkle burst for "recovery x2"
        // and incorrectly assumed redundancy could substitute for the
        // gap (it can't - redundancy of malformed frames is still
        // malformed). 50 ms is comfortably above the decoder's
        // quiet-window threshold AND doubles as the visible-sparkle
        // window: the bracelet renders the sparkle envelope for those
        // 50 ms before the recovery arrives and pre-empts.
        //
        // ifdef ARDUINO so native unit tests don't block.
#ifdef ARDUINO
        ::delay(50);
#endif

        // Recovery: snap back to wash with T_0 attack so the wash
        // colour re-establishes cleanly after the 50 ms sparkle
        // window. Single send is sufficient with the gap restored -
        // the bracelet processes recovery cleanly because it's no
        // longer in mid-frame decode of the sparkle.
        n = pixmob::buildSingleColor(
            buf, kPulseBufSize,
            wr, wg, wb,
            pixmob::T_0_MS,      // instant snap to wash
            pixmob::T_3840_MS,   // hold the wash colour
            pixmob::T_0_MS,      // no release - the next refresh
                                 // pre-empts well before this matters
            pixmob::CHANCE_100,
            group_id);
        if (n != 0) {
            transmit(buf, n);
        }
        (void)t;

        return true;
    }

    // No active wash on this group - regular SingleColor pulse using
    // the orchestrator-supplied envelope. Apply the same chance pre-
    // filter as the wash-active path: skip the sparkle entirely on
    // the StickC instead of asking the bracelet to skip, so the
    // bracelet doesn't accumulate stress from processing-then-
    // skipping (2026-06-30).
    if (ev.chance != pixmob::CHANCE_100) {
#ifdef ARDUINO
        const uint32_t roll = static_cast<uint32_t>(::esp_random());
#else
        const uint32_t roll = static_cast<uint32_t>(std::rand());
#endif
        static constexpr uint8_t kRenderThreshold[8] = {
            255, 224, 170, 128, 82, 41, 26, 10,
        };
        const uint8_t threshold = kRenderThreshold[ev.chance & 0x07];
        if ((roll & 0xFF) >= threshold) {
            return true;
        }
    }

    uint16_t buf[kPulseBufSize];
    const size_t n = pixmob::buildSingleColor(
        buf, kPulseBufSize,
        ev.r, ev.g, ev.b,
        ev.attack, ev.sustain, ev.release,
        pixmob::CHANCE_100,    // chance pre-filtered above
        group_id);
    if (n == 0) return false;

    transmit(buf, n);
    return true;
}

bool PixMobIRDriver::send(uint8_t /*group_id*/, const AssignDeviceGroupEvent& ev) {
    // PixMob group assignment is a 2-command sequence:
    //   1. SetGroupId(slot, new_id)  - writes the value into the EEPROM slot
    //   2. SetGroupSel(slot)         - activates that slot's value as the
    //                                  bracelet's current group filter
    // The first command alone leaves the new value stored but unused.
    // Reference: jamesw343/PixMob_IR pixmob_ir_protocol_examples.py "Group
    // Id" example, where the comment on SetGroupSel reads "this changes
    // the PixMob's group id to 22". The dispatch group_id is not used
    // here - both commands fire as broadcast (restrictGroupId=0) so the
    // target bracelet (physically isolated per protocol) receives them.
    if (ev.new_group_id < 1 || ev.new_group_id > 31) return false;

    uint16_t buf[kPulseBufSize];

    // Step 1: Write new_group_id into slot 0.
    size_t n = pixmob::buildSetGroupId(
        buf, kPulseBufSize,
        /*groupSel=*/0, ev.new_group_id, /*restrictGroupId=*/0);
    if (n == 0) return false;
    transmit(buf, n);

    // Brief gap so the bracelet finishes processing the first command
    // before the second one starts arriving on the wire. 30 ms is well
    // under any user-perceived delay and gives the IR receiver time to
    // re-arm after the trailing pulse of the first transmission.
#ifdef ARDUINO
    ::delay(30);
#endif

    // Step 2: Activate slot 0 - commits new_group_id as the active filter.
    n = pixmob::buildSetGroupSel(
        buf, kPulseBufSize,
        /*groupSel=*/0, /*restrictGroupId=*/0);
    if (n == 0) return false;
    transmit(buf, n);
    return true;
}

// -----------------------------------------------------------------------------
// Wash family (Epic 11). The bracelet has no native wash; we maintain
// per-group state and fire periodic SingleColor refreshes that the bracelet
// renders as continuous held colour. Refresh envelope is square
// (T_0 / T_3840 / T_0) per the bench finding that a release tail produces
// a visible decay between refreshes. See lume-capabilities-design.md §10.
// -----------------------------------------------------------------------------

uint32_t PixMobIRDriver::now_ms() const {
    if (clock_source_) return clock_source_();
#ifdef ARDUINO
    return ::millis();
#else
    // Native test envs without an injected clock get a frozen zero; tests
    // that exercise time-dependent paths install a stub via
    // set_clock_source().
    return 0;
#endif
}

const PixMobIRDriver::WashState*
PixMobIRDriver::wash_state(uint8_t group_id) const {
    if (group_id >= kWashSlots) return nullptr;
    return &wash_slots_[group_id];
}

void PixMobIRDriver::fire_wash_refresh(uint8_t group_id,
                                       const WashState& state,
                                       uint32_t now) {
    // Compute the live colour - blended A↔B for drifting washes, A only
    // for static (cycle_ms == 0). Static washes still go through the
    // blender; it short-circuits to A when cycle is 0.
    uint8_t r = state.r1, g = state.g1, b = state.b1;
    if (state.cycle_ms > 0) {
        compute_drift_rgb(state, now, r, g, b);
    }
    // Snap, hold, snap off (T_0 / T_3840 / T_0). Bench finding
    // 2026-06-18: while the bracelet renders an attack envelope it is
    // mostly deaf to incoming IR commands (sparkles fired during the
    // bracelet's attack window are dropped ~60 % of the time, even
    // with 3x redundancy - the dropouts are fully correlated to
    // attack-render state). Using T_0_MS attack on the periodic
    // refresh keeps the bracelet's busy-window to zero - the
    // bracelet snaps to the snapshot colour and immediately enters
    // its (steady-state, IR-receptive) sustain phase. Cost: the
    // drift between A and B reads as step-wise between snapshots
    // rather than a continuous morph, but step granularity at 3 s
    // refresh against a 5 s drift is visible-but-acceptable for the
    // legacy PixMob hardware Epic 11 targets - the future of crowd
    // lighting on this stack is ESP-NOW Lumes (Tildagon and
    // successor designs), where wash drift renders natively as
    // continuous on the receiver. PixMob is best-effort EMF
    // deployment, not the aesthetic baseline.
    uint16_t buf[kPulseBufSize];
    const size_t n = pixmob::buildSingleColor(
        buf, kPulseBufSize,
        r, g, b,
        pixmob::T_0_MS,
        pixmob::T_3840_MS,
        pixmob::T_0_MS,
        pixmob::CHANCE_100,
        group_id);
    if (n == 0) return;
    transmit(buf, n);
}

void PixMobIRDriver::compute_drift_rgb(const WashState& state,
                                       uint32_t now,
                                       uint8_t& r,
                                       uint8_t& g,
                                       uint8_t& b) const {
    // Linear A↔B↔A blend at the instant of refresh. The refresh fires
    // with T_0_MS attack so the bracelet snaps to whatever colour the
    // Tildagon is currently rendering at refresh time - no need to
    // look ahead to compensate for morph time (the earlier lookahead
    // existed because the refresh used T_2400_MS attack and the
    // bracelet's morph end-position needed to match the Tildagon's
    // position-at-morph-end; with T_0_MS attack there is no morph to
    // compensate for). Visual difference between linear and cosine-
    // eased blend at refresh granularity is sub-perceptual; integer-
    // only math keeps the refresh tick cheap.
    // Snapshot cycle_ms once before using it. send_wash runs on the WiFi
    // task (ESP-NOW recv callback) and can overwrite this slot's
    // cycle_ms with 0 - a routine cue change from a drifting wash to a
    // static colour - while this runs on the main task, in the window
    // between a caller's `cycle_ms > 0` check and the modulo/divide
    // below. Re-reading state.cycle_ms for each operation would then
    // divide by zero, which on Xtensa is a hardware IntegerDivideByZero
    // panic (reboot mid-show). One local load plus a zero guard closes
    // that window: a mid-flight transition to a static wash just holds
    // colour A, exactly as the cycle_ms == 0 path elsewhere does.
    const uint32_t cycle_ms = state.cycle_ms;
    if (cycle_ms == 0) {
        r = state.r1; g = state.g1; b = state.b1;
        return;
    }
    const uint32_t elapsed = (now - state.started_ms) % cycle_ms;
    // phase in 0..1 over one full A↔B↔A oscillation, scaled by 1024
    // to keep it integer-friendly: 0..512 = A→B, 512..1024 = B→A.
    const uint32_t phase_q10 = (elapsed * 1024u) / cycle_ms;
    uint32_t blend_q10;
    if (phase_q10 < 512u) {
        blend_q10 = phase_q10 * 2u;             // 0..1024 as we go A→B
    } else {
        blend_q10 = (1024u - phase_q10) * 2u;   // 1024..0 as we go B→A
    }
    const uint32_t inv = 1024u - blend_q10;
    r = static_cast<uint8_t>((state.r1 * inv + state.r2 * blend_q10) >> 10);
    g = static_cast<uint8_t>((state.g1 * inv + state.g2 * blend_q10) >> 10);
    b = static_cast<uint8_t>((state.b1 * inv + state.b2 * blend_q10) >> 10);
}

uint8_t PixMobIRDriver::quantize_100ms_to_pixmob_time(uint8_t units_100ms) const {
    // pixmob::Time bucket centres in ms: 0, 32, 96, 192, 480, 960, 2400, 3840.
    // Caller passes a wire-side 100 ms-unit value (LightWashEvent.attack /
    // .release, LIGHT_WASH_END.release_time). Returns the pixmob::Time
    // enum value (0..7) of the bucket closest to the requested time.
    static constexpr uint16_t kBucketMs[8] = {
        0, 32, 96, 192, 480, 960, 2400, 3840,
    };
    const uint16_t target = static_cast<uint16_t>(units_100ms) * 100;
    uint8_t best = 0;
    int32_t best_diff = static_cast<int32_t>(kBucketMs[0]) - static_cast<int32_t>(target);
    if (best_diff < 0) best_diff = -best_diff;
    for (uint8_t i = 1; i < 8; ++i) {
        int32_t diff = static_cast<int32_t>(kBucketMs[i]) - static_cast<int32_t>(target);
        if (diff < 0) diff = -diff;
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return best;
}

bool PixMobIRDriver::send_wash(uint8_t target_group, const LightWashEvent& ev) {
    if (target_group >= kWashSlots) return false;

    // Treat all-zero LIGHT_WASH as semantically equivalent to
    // LIGHT_WASH_END. Bench finding 2026-06-30 (user hypothesis,
    // confirmed by sequence analysis): the orchestrator's `stop`
    // cue (Blackout FX) zeros the entire universe, which the StickC's
    // DMX mapper emits as LIGHT_WASH(r=0, g=0, b=0, cycle=0,
    // intensity=0). Firing that as a SingleColor(0,0,0, T_0, T_3840,
    // T_0) refresh on the bracelet (the path before this filter
    // runs) puts the bracelet into a 3.84 s "hold black" envelope.
    // Bracelet appears to enter a stressed / rate-limited state from
    // this and stays there - the 3-second blip pattern observed
    // when music plays follows the `stop` cue at the song's start.
    //
    // Wash Test alone (which doesn't fire stop first) is clean;
    // music (which fires stop first via the cue file) shows the
    // blip; and the bracelet retains the stressed state until power-
    // cycled.
    //
    // Fix: route all-zero washes through send_wash_end with instant
    // cancel. The IR driver stops refreshing, the bracelet's last
    // envelope completes naturally, no black SingleColor lands.
    if (ev.r1 == 0 && ev.g1 == 0 && ev.b1 == 0 &&
        ev.r2 == 0 && ev.g2 == 0 && ev.b2 == 0 &&
        ev.intensity == 0) {
        return send_wash_end(target_group, /*release_time=*/0);
    }

    WashState& s = wash_slots_[target_group];
    const uint32_t now = now_ms();

    // Record the cue's anchors + cycle + attack. ttl_seconds, release,
    // and pulse_response are wire fields that capable Lumes use; PixMob
    // bracelets don't understand them, so they're not stored here.
    // intensity is applied by the bracelet hardware via SingleColor's
    // RGB - we just pass the raw RGB through.
    s.active          = true;
    s.r1 = ev.r1; s.g1 = ev.g1; s.b1 = ev.b1;
    s.r2 = ev.r2; s.g2 = ev.g2; s.b2 = ev.b2;
    s.cycle_ms        = ev.cycle_ms;
    s.attack_100ms    = ev.attack;
    s.started_ms      = now;

    // Refresh cadence scales to the drift cycle so the bracelet's
    // step-wise snapshots produce visibly-smooth blending. Aim for
    // kWashRefreshSnapshotsPerCycle steps per A↔B↔A round-trip;
    // clamp to [kWashRefreshMinMs, kWashRefreshMaxMs] to bound IR
    // airtime and to keep slow-drift washes from refreshing more
    // often than necessary. Static washes (cycle_ms == 0) hold at
    // the max - no drift means no need to refresh fast.
    if (ev.cycle_ms == 0) {
        s.refresh_interval_ms = kWashRefreshMaxMs;
    } else {
        uint32_t interval = static_cast<uint32_t>(ev.cycle_ms)
                                / kWashRefreshSnapshotsPerCycle;
        if (interval < kWashRefreshMinMs) interval = kWashRefreshMinMs;
        if (interval > kWashRefreshMaxMs) interval = kWashRefreshMaxMs;
        s.refresh_interval_ms = interval;
    }
    s.next_refresh_ms = now + s.refresh_interval_ms;

    // Fire the first refresh immediately so the bracelet lights up
    // without waiting kWashRefreshMs.
    fire_wash_refresh(target_group, s, now);
    return true;
}

bool PixMobIRDriver::send_wash_end(uint8_t target_group, uint8_t release_time) {
    if (target_group >= kWashSlots) return false;
    WashState& s = wash_slots_[target_group];
    if (!s.active) return false;

    if (release_time > 0) {
        // Faded cancel: fire SingleColor(black, attack=release_bucket,
        // T_0, T_0). The bracelet morphs from its current rendered
        // colour (the wash) to black over the attack time, then
        // immediately enters T_0 sustain (no hold) and T_0 release
        // (no further fade) - net: a single smooth fade to black,
        // landing at black exactly when the attack completes.
        //
        // Bench history (2026-06-18):
        //   - sustain=T_0_MS with the wash's own colour: bracelet
        //     interpreted as no-op, ignored.
        //   - sustain=T_32_MS with the wash's own colour: worked
        //     against the long-attack-refresh path (T_2400_MS attack)
        //     because the bracelet was usually mid-attack rendering
        //     a colour that visibly differed from the cancel target.
        //     With the T_0_MS snap-on refresh introduced 2026-06-18,
        //     the bracelet sits in sustain matching the cancel target
        //     exactly, and the no-op pattern resurfaces (bench
        //     observation: "cancel wash didn't stop the PixMob; ESP-
        //     NOW lumes faded to black normally").
        //   - attack=release_bucket toward BLACK with T_0 sustain +
        //     T_0 release: the target (black) is unambiguously
        //     different from the current rendered wash colour, so
        //     the bracelet processes the envelope. The attack itself
        //     IS the fade - no need for sustain or release.
        const uint8_t release_bucket = quantize_100ms_to_pixmob_time(release_time);
        uint16_t buf[kPulseBufSize];
        const size_t n = pixmob::buildSingleColor(
            buf, kPulseBufSize,
            0, 0, 0,
            static_cast<pixmob::Time>(release_bucket),
            pixmob::T_0_MS,
            pixmob::T_0_MS,
            pixmob::CHANCE_100,
            target_group);
        if (n != 0) transmit(buf, n);
    }
    // Instant cancel (release_time == 0): just stop refreshing.
    // The bracelet's currently-active refresh envelope (T_3840 sustain,
    // T_0 release) snap-offs naturally at sustain end, giving a clean
    // exit with no further IR traffic from us. Worst-case visible
    // hangover: up to one sustain window (~3.84 s) if the user
    // cancels immediately after a refresh fired.

    s.active = false;
    return true;
}

bool PixMobIRDriver::send_wash_pulse(uint8_t target_group,
                                     const RgbPulseEvent& ev) {
    if (target_group >= kWashSlots) {
        // Fall back to a regular pulse; out-of-range target is unusual
        // but shouldn't crash the rendering path.
        return send(target_group, ev);
    }
    WashState& s = wash_slots_[target_group];
    if (!s.active) {
        // No wash on this group - emit a regular pulse instead.
        // Matches the spec: LIGHT_WASH_PULSE is silently dropped on
        // a non-washing target by capable Lumes; firing a fallback
        // pulse here matches the "kick happens regardless" intent.
        return send(target_group, ev);
    }

    // Composite: TwoColors(sparkle, current_wash_rgb). Bracelet
    // flashes sparkle briefly then holds the wash colour for 384 ms
    // before fading - the protocol-native "kick + tail colour".
    // The 384 ms tail acts as a brief refresh, so we push the next
    // periodic refresh back; if the LD is firing sparkles steadily,
    // they'll suffice to keep the wash alive and our refresh tick
    // only catches the gaps.
    const uint32_t now = now_ms();
    uint8_t wr = s.r1, wg = s.g1, wb = s.b1;
    if (s.cycle_ms > 0) {
        compute_drift_rgb(s, now, wr, wg, wb);
    }
    uint16_t buf[kPulseBufSize];
    const size_t n = pixmob::buildTwoColors(
        buf, kPulseBufSize,
        ev.r, ev.g, ev.b,     // colour 1: the sparkle
        wr, wg, wb);          // colour 2: the wash to return to
    if (n == 0) return false;
    transmit(buf, n);
    s.next_refresh_ms = now + s.refresh_interval_ms;
    return true;
}

void PixMobIRDriver::loop_tick() {
    const uint32_t now = now_ms();
    for (uint8_t g = 0; g < kWashSlots; ++g) {
        WashState& s = wash_slots_[g];
        if (!s.active) continue;
        if (now < s.next_refresh_ms) continue;
        fire_wash_refresh(g, s, now);
        s.next_refresh_ms = now + s.refresh_interval_ms;
    }
}

}  // namespace dal
}  // namespace nocturnation
