// DmxChannelMapper - turns a 12-channel DMX fixture slice into
// LIGHT_PULSE / LIGHT_WASH wire events.
//
// Epic 7 B2. The Director's DmxBridge mode (B3) owns one of these per
// addressed Lume group. Each call to process() walks the current
// channel-value buffer, applies DMX-mode policy (rising-edge trigger
// detection, anchor-change wash debounce, strobe-rate cadence, master
// intensity scaling), and emits typed events through the supplied
// Sink. The mapper itself never reaches DAL or the wire - the Sink
// adapter (constructed by B3) calls DAL::render_fx / render_wash.
//
// Channel layout (Epic 7 B0, 12-channel fixture per Lume group):
//
//   Ch 1  Master intensity   (0..255 scalar applied to pulse RGB +
//                             wash intensity field)
//   Ch 2  Strobe rate        (0 = off; 1..255 maps linearly to
//                             0..4 Hz continuous pulse cadence)
//   Ch 3  Pulse R
//   Ch 4  Pulse G
//   Ch 5  Pulse B
//   Ch 6  Pulse trigger      (0..127 = idle; 128..255 = fire one
//                             pulse on the rising edge into the
//                             fire band - held high doesn't re-
//                             fire until value drops back into idle)
//   Ch 7  Wash anchor A R
//   Ch 8  Wash anchor A G
//   Ch 9  Wash anchor A B
//   Ch 10 Wash anchor B R    (= A for a static single-colour wash)
//   Ch 11 Wash anchor B G
//   Ch 12 Wash anchor B B
//
// DMX-mode defaults held at fixed values (LD drives motion via
// console channel automation, not internal wash cycling):
//
//   cycle_ms        = 0           (hold anchor A; no internal drift)
//   pulse chance    = CHANCE_100  (every Lume fires on every trigger)
//   pulse ASR       = T_0 / T_96 / T_192   (Bass & Drift defaults)
//   wash attack     = 20          (2.0 s ramp-in)
//   wash release    = 10          (1.0 s default fade)
//   pulse_response  = 1           (overlays welcome)
//
// The mapper itself does NOT emit LIGHT_WASH_END. Mode exit emits it
// at B3 (when the bridge tears down).

#pragma once

#include <cstddef>
#include <cstdint>

#include "dal/dal.h"

namespace nocturnation {
namespace dal {

class DmxChannelMapper {
public:
    // Required number of channels per fixture slice (Epic 7 B0).
    static constexpr uint16_t kChannelsPerGroup = 12;

    // 0-based indices into the 12-channel slice; documented here so
    // callers + tests can reach them by name instead of magic numbers.
    enum ChannelIdx : uint8_t {
        kMasterIntensity = 0,
        kStrobeRate      = 1,
        kPulseR          = 2,
        kPulseG          = 3,
        kPulseB          = 4,
        kPulseTrigger    = 5,
        kWashAR          = 6,
        kWashAG          = 7,
        kWashAB          = 8,
        kWashBR          = 9,
        kWashBG          = 10,
        kWashBB          = 11,
    };

    // Pulse-trigger threshold: a rising edge into the [kTriggerHi, 255]
    // band fires one pulse; the value must drop back below
    // kTriggerLo before another rising edge can re-fire.
    static constexpr uint8_t kTriggerHi = 128;
    static constexpr uint8_t kTriggerLo = 128;   // single threshold; no hysteresis in v1

    // Safety floor inherited from architecture spec §15.1. Strobe
    // channel maps to [0, kMaxStrobeHz] linearly.
    static constexpr float   kMaxStrobeHz       = 4.0f;
    static constexpr uint32_t kMinStrobeIntervalMs = 250;  // 1000 / 4 Hz

    // Wash re-emit debounce floor: even if anchors change every frame
    // the mapper won't emit faster than this.
    static constexpr uint32_t kMinWashEmitGapMs = 50;

    // Sink that the mapper calls to deliver events. Concrete sinks at
    // B3 forward to DAL::render_fx / render_wash with the right target
    // string; tests inject recording sinks.
    class Sink {
    public:
        virtual ~Sink() = default;
        virtual void on_pulse(const char* target, const RgbPulseEvent& ev) = 0;
        virtual void on_wash (const char* target, const LightWashEvent& ev) = 0;
    };

    DmxChannelMapper();

    // Reset all per-group state. Does NOT change the target string.
    void reset();

    // Set the target string for events emitted by this mapper. Caller
    // owns the string memory; the mapper holds the pointer. Typical
    // values: "00:00" (broadcast), "01:01" (Light class, group 1).
    void set_target(const char* target) { target_ = target; }
    const char* target() const { return target_; }

    // Process the current channel state. `channels` must point to at
    // least kChannelsPerGroup bytes; shorter buffers are ignored
    // (process() becomes a no-op). `now_ms` drives strobe cadence +
    // wash debounce.
    void process(const uint8_t* channels,
                 uint16_t       channel_count,
                 uint32_t       now_ms,
                 Sink&          sink);

private:
    const char* target_ = "00:00";

    // Rising-edge detection on pulse trigger.
    bool    pulse_armed_   = true;     // true = next high crossing fires
    uint8_t last_trigger_  = 0;

    // Wash change detection. The mapper holds the last anchors it
    // emitted to the wire (plus master intensity that produced them)
    // and re-emits when anything changes.
    bool    wash_seeded_   = false;    // first-call gate: emit even at all-zero
    uint8_t last_anchor_r1_ = 0, last_anchor_g1_ = 0, last_anchor_b1_ = 0;
    uint8_t last_anchor_r2_ = 0, last_anchor_g2_ = 0, last_anchor_b2_ = 0;
    uint8_t last_emitted_intensity_ = 0;
    uint32_t last_wash_emit_ms_ = 0;

    // Strobe channel cadence tracking.
    uint8_t  last_strobe_rate_  = 0;
    uint32_t last_strobe_fire_ms_ = 0;
    uint32_t next_strobe_fire_ms_ = 0;

    // Internal helpers.
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

    // Build a "fire pulse" event with the current pulse RGB and
    // DMX-mode defaults. Master intensity scales RGB.
    RgbPulseEvent build_pulse_event(const uint8_t* ch, uint8_t master) const;
};

}  // namespace dal
}  // namespace nocturnation
