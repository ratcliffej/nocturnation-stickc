// DmxChannelMapper - turns one 49/50-channel DMX block into
// LIGHT_PULSE / LIGHT_WASH wire events for one `target_group`.
//
// Epic 7 B7 (per-group + extended-parameter surface). Each mapper
// instance owns ONE block of the DMX universe:
//
//   Universe addresses 1-49      -> mapper(0)  -> target_group=0 (broadcast)
//   Universe addresses 50-99     -> mapper(1)  -> target_group=1
//   Universe addresses 100-149   -> mapper(2)  -> target_group=2
//   ...
//   Universe addresses 450-499   -> mapper(9)  -> target_group=9
//
// DmxBridgeMode (B3) owns ten instances and iterates them on each
// DMX frame; per-block state means a change in Group 1's slice only
// causes a Group 1 emission, not Groups 2-9. Per-block change
// detection + the kMinWashEmitGapMs debounce keep wire traffic
// proportional to actual operator activity.
//
// Per-block channel layout (offsets within the block; the block's
// channel 1 is offset 0):
//
//   Off  Group    Name                  Maps to wire field
//   ---  ------   --------------------  --------------------------------
//   0    Global   Master Intensity      brightness scalar on all RGB
//   1    Global   Strobe Rate           1..255 -> 0..4 Hz pulse cadence
//                                       (preserved from v1; no-op at 0)
//   2    Pulse    Pulse R               LIGHT_PULSE.r
//   3    Pulse    Pulse G               LIGHT_PULSE.g
//   4    Pulse    Pulse B               LIGHT_PULSE.b
//   5    Pulse    Pulse Trigger         rising edge past 128 fires
//   6    Pulse    Pulse Attack          pixmob::Time enum (slider->idx)
//   7    Pulse    Pulse Sustain         pixmob::Time enum
//   8    Pulse    Pulse Release         pixmob::Time enum
//   9    Pulse    Pulse Probability     pixmob::Chance enum (inverted -
//                                       low slider = low chance)
//   10   Wash A   Wash A R              LIGHT_WASH.r1
//   11   Wash A   Wash A G              LIGHT_WASH.g1
//   12   Wash A   Wash A B              LIGHT_WASH.b1
//   13   Wash B   Wash B R              LIGHT_WASH.r2
//   14   Wash B   Wash B G              LIGHT_WASH.g2
//   15   Wash B   Wash B B              LIGHT_WASH.b2
//   16   Wash     Wash Cycle            100 ms units; 0=hold A, 1..255
//                                       = 100ms..25.5s A<->B<->A
//   17   Wash     Wash Intensity        LIGHT_WASH.intensity
//   18   Wash     Wash Attack           100 ms units
//   19   Wash     Wash Release          100 ms units
//   20   Wash     Wash TTL Lo           u16 LE seconds; 0 = infinite
//   21   Wash     Wash TTL Hi
//   22   Wash     Wash Pulse Response   >=128 enables PULSE on wash
//   23-49 reserved                       Future expansion - ignored
//
// LIGHT_WASH_END is not emitted from inside the mapper; B3 emits it
// on mode exit so the fleet's release behaviour is consistent.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dal/dal.h"
#include "pulse/envelope.h"

namespace nocturnation {
namespace dal {

class DmxChannelMapper {
public:
    // A "block" is the LD-addressable unit: the broadcast block is 49
    // channels (DMX is 1-indexed; address 1-49 = 49 addresses) and each
    // group block is 50 channels (addresses 50-99, 100-149, ...). The
    // mapper reads up to kActiveChannelsPerBlock; channels past that
    // are reserved for future expansion and silently ignored.
    //
    // Channel 23 (kRawR..kRawEnable, slot 23-26 zero-indexed) is the
    // raw-RGB path added at the EMF stage team's request - it lets an
    // LD treat each block as a dumb 3-channel RGB fixture, bypassing
    // the FX engine entirely. Active when kRawEnable >= kRawEnableHi.
    static constexpr uint16_t kActiveChannelsPerBlock = 27;

    enum BlockChannel : uint8_t {
        kMasterIntensity   = 0,
        kStrobeRate        = 1,
        kPulseR            = 2,
        kPulseG            = 3,
        kPulseB            = 4,
        kPulseTrigger      = 5,
        kPulseAttack       = 6,
        kPulseSustain      = 7,
        kPulseRelease      = 8,
        kPulseProbability  = 9,
        kWashAR            = 10,
        kWashAG            = 11,
        kWashAB            = 12,
        kWashBR            = 13,
        kWashBG            = 14,
        kWashBB            = 15,
        kWashCycle         = 16,
        kWashIntensity     = 17,
        kWashAttack        = 18,
        kWashRelease       = 19,
        kWashTtlLo         = 20,
        kWashTtlHi         = 21,
        kWashPulseResponse = 22,
        // Raw RGB + enable (EMF artist-stage request 2026-06-23). When
        // kRawEnable is high, the mapper emits a static LIGHT_WASH from
        // kRawR/G/B (scaled by kMasterIntensity, standard DMX fixture
        // convention) and suppresses the FX engine's pulse/wash output
        // for this block. When kRawEnable is low, the FX engine path
        // is active and raw channels are ignored.
        kRawR              = 23,
        kRawG              = 24,
        kRawB              = 25,
        kRawEnable         = 26,
    };

    // Raw-mode gate. Matches the convention used by kTriggerHi/Lo and
    // kWashPulseResponse: 0..127 = off, 128..255 = on. Single threshold
    // (no separate Hi/Lo because there's no rising-edge detection -
    // raw mode is a continuous state, not an event).
    static constexpr uint8_t kRawEnableThreshold = 128;

    // Pulse-trigger threshold. Rising edge into [kTriggerHi, 255] fires
    // one pulse; channel must drop below kTriggerLo before re-arming.
    static constexpr uint8_t kTriggerHi = 128;
    static constexpr uint8_t kTriggerLo = 128;

    // Safety floor inherited from architecture spec section 15.1. Strobe
    // channel value 1..255 maps to ~0..4 Hz pulse cadence.
    static constexpr float    kMaxStrobeHz          = 4.0f;
    static constexpr uint32_t kMinStrobeIntervalMs  = 250;

    // Wash re-emit debounce floor: even if anchors change every frame
    // the mapper won't emit faster than this.
    static constexpr uint32_t kMinWashEmitGapMs = 50;

    // Raw-mode wash debounce (EMF stage-team feature). Wider than the
    // FX-wash debounce because an LD dragging an RGB slider in QLC+
    // generates 30+ Hz of universe writes - each one a different raw
    // colour - and rapid wash-frame broadcasts at that rate were
    // overloading the Lume Sticks' receive path enough to crash them
    // (bench 2026-06-23, USER: "StickC in Lume mode keeps crashing
    // occasionally when the sliders are moved"). 200 ms = 5 Hz max
    // is comfortable for the cue-based show workflow (LD presses a
    // button -> one emission) and survives a slider drag without
    // killing Lumes. Strip rendering on the Lume still looks smooth
    // because the wash state machine interpolates between updates.
    static constexpr uint32_t kMinRawWashEmitGapMs = 200;

    // Sink that the mapper calls to deliver events. Concrete sinks at
    // B3 forward to DAL::render_fx / render_wash with the target
    // string the mapper provides ("00:00" for broadcast, "00:01" for
    // group 1, ...).
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual void on_pulse(const char* target, const RgbPulseEvent& ev) = 0;
        virtual void on_wash (const char* target, const LightWashEvent& ev) = 0;
    };

    // Construct for a specific target_group. 0 = broadcast (reaches
    // every Lume regardless of configured group); 1..9 = matching
    // group_id only. Builds the target string "00:XX" once.
    explicit DmxChannelMapper(uint8_t target_group);

    // Reset per-block state. Does NOT change target_group.
    void reset();

    // Process this block's current channel state. `block` points at
    // the slice's first channel (zero-indexed); `channel_count` is the
    // slice size. Slices shorter than kActiveChannelsPerBlock are a
    // no-op. `now_ms` drives strobe cadence + wash debounce.
    void process(const uint8_t* block,
                 uint16_t       channel_count,
                 uint32_t       now_ms,
                 Sink&          sink);

    uint8_t     target_group() const { return target_group_; }
    const char* target()       const { return target_; }

    // Quantizers (exposed for unit tests). Probability is INVERTED so
    // a higher slider position means a higher fire chance, matching
    // LD muscle memory; the on-wire pixmob::Chance enum runs the
    // other way (CHANCE_100 at value 0, CHANCE_4 at value 7).
    static pixmob::Time   quantize_time  (uint8_t slider);
    static pixmob::Chance quantize_chance(uint8_t slider);

    // Steady-state colour this block currently commands, using the same
    // scaling rules as the emit paths: raw RGB (master-scaled, min-1
    // floor) when kRawEnable is high, otherwise wash anchor A scaled by
    // intensity x master. Pure and stateless - transients (pulse,
    // strobe, wash A<->B cycling) are not modelled, so this is "what a
    // Lume settles on", not a frame-accurate replay. Drives the AtomS3R
    // Director's on-screen fleet-colour box. Short/null slices yield
    // black.
    static void preview_rgb(const uint8_t* block, uint16_t channel_count,
                            uint8_t& r, uint8_t& g, uint8_t& b);

private:
    uint8_t target_group_;
    char    target_[6];  // "00:XX\0"

    // Rising-edge detection on pulse trigger.
    bool    pulse_armed_  = true;
    uint8_t last_trigger_ = 0;

    // Wash change detection. Holds the last on-wire values; re-emit
    // when ANY of these differs from the current channel slice.
    bool    wash_seeded_              = false;
    uint8_t last_anchor_r1_           = 0,  last_anchor_g1_ = 0, last_anchor_b1_ = 0;
    uint8_t last_anchor_r2_           = 0,  last_anchor_g2_ = 0, last_anchor_b2_ = 0;
    uint8_t last_wash_intensity_      = 0;
    uint8_t last_wash_attack_         = 0;
    uint8_t last_wash_release_        = 0;
    uint8_t last_wash_cycle_          = 0;
    uint8_t last_wash_ttl_lo_         = 0;
    uint8_t last_wash_ttl_hi_         = 0;
    uint8_t last_wash_pulse_response_ = 0;
    uint32_t last_wash_emit_ms_       = 0;

    // Strobe cadence tracking.
    uint8_t  last_strobe_rate_     = 0;
    uint32_t next_strobe_fire_ms_  = 0;

    // Raw RGB tracking (EMF stage team path). Holds the last-emitted
    // raw colour + the last-seen enable state so we can detect both
    // colour changes (re-emit static wash) and enable edges (emit
    // wash_end when raw mode turns off so the bracelet / strip / LCD
    // drops the static colour cleanly instead of holding stale).
    bool    raw_active_     = false;
    uint8_t last_raw_r_     = 0;
    uint8_t last_raw_g_     = 0;
    uint8_t last_raw_b_     = 0;

    // Helpers.
    void maybe_emit_raw_wash_on_change(const uint8_t* ch,
                                       uint8_t master,
                                       uint32_t now_ms,
                                       Sink& sink);
    void maybe_emit_pulse_on_trigger(const uint8_t* ch,
                                     uint8_t master,
                                     Sink& sink);
    void maybe_emit_wash_on_change(const uint8_t* ch,
                                   uint8_t master,
                                   uint32_t now_ms,
                                   Sink& sink);
    void maybe_emit_strobe(const uint8_t* ch,
                           uint8_t master,
                           uint32_t now_ms,
                           Sink& sink);

    // Build a "fire pulse" event using the LD's pulse RGB + per-block
    // envelope (attack/sustain/release/probability) channels. Master
    // scales the RGB.
    RgbPulseEvent build_pulse_event(const uint8_t* ch, uint8_t master) const;
};

}  // namespace dal
}  // namespace nocturnation
