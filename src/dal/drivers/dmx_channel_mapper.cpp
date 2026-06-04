// DmxChannelMapper implementation - Epic 7 B2.

#include "dmx_channel_mapper.h"

#include "pulse/envelope.h"

namespace nocturnation {
namespace dal {

namespace {

// Scale RGB by master intensity (0..255) into the output channel. Done
// in the integer domain to avoid float overhead on the M5: result =
// (channel * master) / 255. Rounds toward zero.
inline uint8_t scale_byte(uint8_t channel, uint8_t master) {
    return static_cast<uint8_t>((static_cast<uint16_t>(channel)
                                 * static_cast<uint16_t>(master)) / 255);
}

}  // namespace

DmxChannelMapper::DmxChannelMapper() = default;

void DmxChannelMapper::reset() {
    pulse_armed_              = true;
    last_trigger_             = 0;
    wash_seeded_              = false;
    last_anchor_r1_           = 0;
    last_anchor_g1_           = 0;
    last_anchor_b1_           = 0;
    last_anchor_r2_           = 0;
    last_anchor_g2_           = 0;
    last_anchor_b2_           = 0;
    last_emitted_intensity_   = 0;
    last_wash_emit_ms_        = 0;
    last_strobe_rate_         = 0;
    last_strobe_fire_ms_      = 0;
    next_strobe_fire_ms_      = 0;
}

void DmxChannelMapper::process(const uint8_t* channels,
                                uint16_t       channel_count,
                                uint32_t       now_ms,
                                Sink&          sink) {
    if (channels == nullptr || channel_count < kChannelsPerGroup) {
        // Defensive: a short or null buffer can't be interpreted.
        // Better to be a no-op than to read past it.
        return;
    }
    const uint8_t master = channels[kMasterIntensity];

    maybe_emit_wash_on_change(channels, master, now_ms, sink);
    maybe_emit_pulse_on_trigger(channels, master, sink);
    maybe_emit_strobe(channels, master, now_ms, sink);
}

RgbPulseEvent DmxChannelMapper::build_pulse_event(const uint8_t* ch,
                                                   uint8_t master) const {
    RgbPulseEvent ev{};
    ev.r = scale_byte(ch[kPulseR], master);
    ev.g = scale_byte(ch[kPulseG], master);
    ev.b = scale_byte(ch[kPulseB], master);
    ev.attack  = pulse::T_0_MS;
    ev.sustain = pulse::T_96_MS;
    ev.release = pulse::T_192_MS;
    ev.chance  = pulse::CHANCE_100;
    return ev;
}

void DmxChannelMapper::maybe_emit_pulse_on_trigger(const uint8_t* ch,
                                                    uint8_t master,
                                                    Sink& sink) {
    const uint8_t trig = ch[kPulseTrigger];

    // Hysteresis: must dip below kTriggerLo (which equals kTriggerHi in
    // v1 - no hysteresis band yet) before re-arming. Held-high stays
    // disarmed until the LD pulls the channel back down.
    if (trig < kTriggerLo) {
        pulse_armed_ = true;
    }

    const bool rising_edge = pulse_armed_ && trig >= kTriggerHi;
    if (rising_edge) {
        const RgbPulseEvent ev = build_pulse_event(ch, master);
        sink.on_pulse(target_, ev);
        pulse_armed_ = false;
    }

    last_trigger_ = trig;
}

void DmxChannelMapper::maybe_emit_wash_on_change(const uint8_t* ch,
                                                  uint8_t master,
                                                  uint32_t now_ms,
                                                  Sink& sink) {
    const uint8_t r1 = ch[kWashAR];
    const uint8_t g1 = ch[kWashAG];
    const uint8_t b1 = ch[kWashAB];
    const uint8_t r2 = ch[kWashBR];
    const uint8_t g2 = ch[kWashBG];
    const uint8_t b2 = ch[kWashBB];

    const bool anchors_changed =
        (r1 != last_anchor_r1_) || (g1 != last_anchor_g1_) || (b1 != last_anchor_b1_) ||
        (r2 != last_anchor_r2_) || (g2 != last_anchor_g2_) || (b2 != last_anchor_b2_) ||
        (master != last_emitted_intensity_);

    if (!wash_seeded_ || anchors_changed) {
        // Debounce: don't emit faster than kMinWashEmitGapMs even if
        // the LD wiggles a channel rapidly. The first emit bypasses
        // the debounce so a fresh mode entry doesn't have to wait.
        if (wash_seeded_ && (now_ms - last_wash_emit_ms_) < kMinWashEmitGapMs) {
            return;
        }

        LightWashEvent ev{};
        ev.r1 = r1; ev.g1 = g1; ev.b1 = b1;
        ev.r2 = r2; ev.g2 = g2; ev.b2 = b2;
        ev.attack         = 20;        // 2.0 s ramp-in (B0 default)
        ev.release        = 10;        // 1.0 s default fade
        ev.intensity      = master;    // master intensity goes into the wash scalar
        ev.cycle_ms       = 0;         // LD drives motion; no internal cycling
        ev.ttl_seconds    = 0;         // infinite; mode exit emits LIGHT_WASH_END
        ev.pulse_response = 1;         // overlays accepted
        sink.on_wash(target_, ev);

        last_anchor_r1_ = r1; last_anchor_g1_ = g1; last_anchor_b1_ = b1;
        last_anchor_r2_ = r2; last_anchor_g2_ = g2; last_anchor_b2_ = b2;
        last_emitted_intensity_ = master;
        last_wash_emit_ms_      = now_ms;
        wash_seeded_            = true;
    }
}

void DmxChannelMapper::maybe_emit_strobe(const uint8_t* ch,
                                          uint8_t master,
                                          uint32_t now_ms,
                                          Sink& sink) {
    const uint8_t rate = ch[kStrobeRate];

    if (rate == 0) {
        // Strobe disabled. Clear the cadence state so a future rate
        // change fires immediately rather than after the leftover gap.
        last_strobe_rate_    = 0;
        next_strobe_fire_ms_ = 0;
        return;
    }

    // Linear map: value 1..255 -> 63750/value ms interval (1 Hz at
    // ~64, 2 Hz at ~128, 4 Hz at 255). Capped at the safety floor.
    uint32_t interval_ms = 63750u / static_cast<uint32_t>(rate);
    if (interval_ms < kMinStrobeIntervalMs) interval_ms = kMinStrobeIntervalMs;

    // Rate change: fire immediately so the LD's intent is visible
    // straight away. Otherwise fire when the previously-computed
    // next_strobe_fire_ms_ deadline is met.
    const bool rate_changed = (rate != last_strobe_rate_);
    const bool deadline_met = (next_strobe_fire_ms_ != 0)
                              && (now_ms >= next_strobe_fire_ms_);
    if (rate_changed || deadline_met) {
        const RgbPulseEvent ev = build_pulse_event(ch, master);
        sink.on_pulse(target_, ev);
        last_strobe_fire_ms_ = now_ms;
        next_strobe_fire_ms_ = now_ms + interval_ms;
    } else if (next_strobe_fire_ms_ == 0) {
        // First time we see a non-zero rate without rate_changed
        // (e.g., the strobe channel started at the current value).
        // Set the deadline so future calls fire on schedule.
        next_strobe_fire_ms_ = now_ms + interval_ms;
    }
    last_strobe_rate_ = rate;
}

}  // namespace dal
}  // namespace nocturnation
