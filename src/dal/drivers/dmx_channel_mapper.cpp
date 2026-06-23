// DmxChannelMapper implementation - Epic 7 B7.

#include "dmx_channel_mapper.h"

#include <cstdio>

namespace nocturnation {
namespace dal {

namespace {

// Scale RGB by master intensity (0..255) in the integer domain.
// result = (channel * master) / 255. Rounds toward zero.
inline uint8_t scale_byte(uint8_t channel, uint8_t master) {
    return static_cast<uint8_t>((static_cast<uint16_t>(channel)
                                 * static_cast<uint16_t>(master)) / 255);
}

}  // namespace

// ---------------------------------------------------------------------
// Static quantizers
// ---------------------------------------------------------------------

pixmob::Time DmxChannelMapper::quantize_time(uint8_t slider) {
    // 8 enum values, 256 slider positions -> 32 per bucket.
    // slider 0..31 -> T_0_MS, 32..63 -> T_32_MS, ..., 224..255 -> T_3840_MS.
    const uint8_t idx = slider / 32;
    return static_cast<pixmob::Time>(idx);
}

pixmob::Chance DmxChannelMapper::quantize_chance(uint8_t slider) {
    // Inverted: high slider = high chance, but the pixmob::Chance enum
    // numbers CHANCE_100 = 0 and CHANCE_4 = 7. So invert the slider
    // first, then bucket. slider 224..255 -> CHANCE_100; 0..31 -> CHANCE_4.
    const uint8_t inv = static_cast<uint8_t>(255 - slider);
    const uint8_t idx = inv / 32;
    return static_cast<pixmob::Chance>(idx);
}

// ---------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------

DmxChannelMapper::DmxChannelMapper(uint8_t target_group)
    : target_group_(target_group) {
    // target_class is held at 0 (all classes) by the DMX bridge - it
    // doesn't distinguish PixMob vs Tildagon vs Stick on the wire,
    // the receivers decide per-class capability.
    std::snprintf(target_, sizeof(target_), "00:%02X",
                  static_cast<unsigned>(target_group_));
}

void DmxChannelMapper::reset() {
    pulse_armed_               = true;
    last_trigger_              = 0;
    wash_seeded_               = false;
    last_anchor_r1_            = 0; last_anchor_g1_ = 0; last_anchor_b1_ = 0;
    last_anchor_r2_            = 0; last_anchor_g2_ = 0; last_anchor_b2_ = 0;
    last_wash_intensity_       = 0;
    last_wash_attack_          = 0;
    last_wash_release_         = 0;
    last_wash_cycle_           = 0;
    last_wash_ttl_lo_          = 0;
    last_wash_ttl_hi_          = 0;
    last_wash_pulse_response_  = 0;
    last_wash_emit_ms_         = 0;
    last_strobe_rate_          = 0;
    next_strobe_fire_ms_       = 0;
    raw_active_                = false;
    last_raw_r_                = 0;
    last_raw_g_                = 0;
    last_raw_b_                = 0;
}

// ---------------------------------------------------------------------
// Frame processing
// ---------------------------------------------------------------------

void DmxChannelMapper::process(const uint8_t* block,
                               uint16_t       channel_count,
                               uint32_t       now_ms,
                               Sink&          sink) {
    if (block == nullptr || channel_count < kActiveChannelsPerBlock) {
        // Short or null slice can't be safely interpreted; better to
        // no-op than read past the buffer.
        return;
    }
    const uint8_t master  = block[kMasterIntensity];
    const uint8_t enable  = block[kRawEnable];
    const bool raw_request = (enable >= kRawEnableThreshold);

    if (raw_request) {
        // EMF stage-team raw-RGB path. The LD is treating this block
        // as a dumb 3-channel RGB fixture; FX engine output is
        // suppressed entirely for this tick. Pulse, FX wash, and
        // strobe all skipped. pulse_armed_ is held disarmed so a
        // pulse trigger sitting high from a previous FX scene doesn't
        // fire the moment raw mode disengages.
        maybe_emit_raw_wash_on_change(block, master, now_ms, sink);
        pulse_armed_ = false;
        return;
    }

    if (raw_active_) {
        // Transition out of raw mode. Reseed the FX wash detector so
        // the next emit fires fresh from FX channel state, even if
        // those channels haven't changed since before raw mode
        // engaged - otherwise the receivers would keep showing the
        // last raw colour until something in the FX block actually
        // moved. Set once per high->low edge.
        raw_active_  = false;
        wash_seeded_ = false;
    }

    maybe_emit_wash_on_change(block, master, now_ms, sink);
    maybe_emit_pulse_on_trigger(block, master, sink);
    maybe_emit_strobe(block, master, now_ms, sink);
}

void DmxChannelMapper::maybe_emit_raw_wash_on_change(const uint8_t* ch,
                                                     uint8_t master,
                                                     uint32_t now_ms,
                                                     Sink& sink) {
    // Master-scale per standard DMX fixture convention - the LD's
    // Master slider dims the raw colour. Stage team's mental model
    // matches what they'd expect from a plain RGB par.
    const uint8_t scaled_r = scale_byte(ch[kRawR], master);
    const uint8_t scaled_g = scale_byte(ch[kRawG], master);
    const uint8_t scaled_b = scale_byte(ch[kRawB], master);

    const bool first_entry = !raw_active_;
    const bool changed     = (scaled_r != last_raw_r_)
                          || (scaled_g != last_raw_g_)
                          || (scaled_b != last_raw_b_);

    if (!first_entry && !changed) {
        return;  // steady state - nothing to emit
    }

    // Reuse the same kMinWashEmitGapMs debounce the FX wash path uses
    // so an LD strobing the raw channels via cue chases can't flood
    // the wire.
    if (!first_entry && (now_ms - last_wash_emit_ms_) < kMinWashEmitGapMs) {
        return;
    }

    LightWashEvent ev{};
    ev.r1 = scaled_r;  ev.g1 = scaled_g;  ev.b1 = scaled_b;
    ev.r2 = scaled_r;  ev.g2 = scaled_g;  ev.b2 = scaled_b;
    ev.attack         = 0;     // 0 ms - instant snap to colour
    ev.release        = 0;     // 0 ms - clean end if cancelled
    ev.intensity      = 255;   // raw R/G/B already master-scaled
    ev.cycle_ms       = 0;     // static; A == B so no drift either way
    ev.ttl_seconds    = 0;     // infinite (held until next change)
    ev.pulse_response = 1;     // allow pulse overlay (irrelevant in raw
                               // mode since FX pulses are suppressed,
                               // but harmless for a future ad-hoc cue)
    sink.on_wash(target_, ev);

    raw_active_        = true;
    last_raw_r_        = scaled_r;
    last_raw_g_        = scaled_g;
    last_raw_b_        = scaled_b;
    last_wash_emit_ms_ = now_ms;
}

// ---------------------------------------------------------------------
// Pulse event builder
// ---------------------------------------------------------------------

RgbPulseEvent DmxChannelMapper::build_pulse_event(const uint8_t* ch,
                                                  uint8_t master) const {
    RgbPulseEvent ev{};
    ev.r       = scale_byte(ch[kPulseR], master);
    ev.g       = scale_byte(ch[kPulseG], master);
    ev.b       = scale_byte(ch[kPulseB], master);
    ev.attack  = quantize_time  (ch[kPulseAttack]);
    ev.sustain = quantize_time  (ch[kPulseSustain]);
    ev.release = quantize_time  (ch[kPulseRelease]);
    ev.chance  = quantize_chance(ch[kPulseProbability]);
    return ev;
}

// ---------------------------------------------------------------------
// Pulse / trigger
// ---------------------------------------------------------------------

void DmxChannelMapper::maybe_emit_pulse_on_trigger(const uint8_t* ch,
                                                   uint8_t master,
                                                   Sink& sink) {
    const uint8_t trig = ch[kPulseTrigger];

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

// ---------------------------------------------------------------------
// Wash family
// ---------------------------------------------------------------------

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

    // Wash intensity = (channel intensity) * (master scalar) / 255.
    // Lets the LD set per-block wash brightness independently of the
    // global master, while still respecting master as an overall dim.
    const uint8_t wash_intensity =
        scale_byte(ch[kWashIntensity], master);

    // Cycle channel is 100 ms units. 0 -> hold A; 1..255 -> 100..25500 ms.
    const uint8_t  cycle_raw = ch[kWashCycle];
    const uint16_t cycle_ms  = static_cast<uint16_t>(cycle_raw) * 100;

    const uint8_t attack_100ms  = ch[kWashAttack];
    const uint8_t release_100ms = ch[kWashRelease];

    // TTL is a 16-bit LE pair on the wire; QLC+ exposes it as two
    // channels (Lo + Hi) since 50ms-resolution single-byte (0-25.5s)
    // wouldn't cover the LD's want of "10-minute auto-end" for
    // unattended runs.
    const uint8_t  ttl_lo = ch[kWashTtlLo];
    const uint8_t  ttl_hi = ch[kWashTtlHi];
    const uint16_t ttl_seconds =
        static_cast<uint16_t>(ttl_lo)
        | (static_cast<uint16_t>(ttl_hi) << 8);

    const uint8_t pulse_response = (ch[kWashPulseResponse] >= 128) ? 1 : 0;

    const bool changed =
        (r1 != last_anchor_r1_) || (g1 != last_anchor_g1_) || (b1 != last_anchor_b1_) ||
        (r2 != last_anchor_r2_) || (g2 != last_anchor_g2_) || (b2 != last_anchor_b2_) ||
        (wash_intensity   != last_wash_intensity_)        ||
        (attack_100ms     != last_wash_attack_)           ||
        (release_100ms    != last_wash_release_)          ||
        (cycle_raw        != last_wash_cycle_)            ||
        (ttl_lo           != last_wash_ttl_lo_)           ||
        (ttl_hi           != last_wash_ttl_hi_)           ||
        (pulse_response   != last_wash_pulse_response_);

    if (!wash_seeded_ || changed) {
        if (wash_seeded_ && (now_ms - last_wash_emit_ms_) < kMinWashEmitGapMs) {
            return;
        }

        LightWashEvent ev{};
        ev.r1 = r1; ev.g1 = g1; ev.b1 = b1;
        ev.r2 = r2; ev.g2 = g2; ev.b2 = b2;
        ev.attack         = attack_100ms;
        ev.release        = release_100ms;
        ev.intensity      = wash_intensity;
        ev.cycle_ms       = cycle_ms;
        ev.ttl_seconds    = ttl_seconds;
        ev.pulse_response = pulse_response;
        sink.on_wash(target_, ev);

        last_anchor_r1_           = r1; last_anchor_g1_ = g1; last_anchor_b1_ = b1;
        last_anchor_r2_           = r2; last_anchor_g2_ = g2; last_anchor_b2_ = b2;
        last_wash_intensity_      = wash_intensity;
        last_wash_attack_         = attack_100ms;
        last_wash_release_        = release_100ms;
        last_wash_cycle_          = cycle_raw;
        last_wash_ttl_lo_         = ttl_lo;
        last_wash_ttl_hi_         = ttl_hi;
        last_wash_pulse_response_ = pulse_response;
        last_wash_emit_ms_        = now_ms;
        wash_seeded_              = true;
    }
}

// ---------------------------------------------------------------------
// Strobe (preserved from v1)
// ---------------------------------------------------------------------

void DmxChannelMapper::maybe_emit_strobe(const uint8_t* ch,
                                          uint8_t master,
                                          uint32_t now_ms,
                                          Sink& sink) {
    const uint8_t rate = ch[kStrobeRate];

    if (rate == 0) {
        last_strobe_rate_    = 0;
        next_strobe_fire_ms_ = 0;
        return;
    }

    uint32_t interval_ms = 63750u / static_cast<uint32_t>(rate);
    if (interval_ms < kMinStrobeIntervalMs) interval_ms = kMinStrobeIntervalMs;

    const bool rate_changed = (rate != last_strobe_rate_);
    const bool deadline_met = (next_strobe_fire_ms_ != 0)
                              && (now_ms >= next_strobe_fire_ms_);
    if (rate_changed || deadline_met) {
        const RgbPulseEvent ev = build_pulse_event(ch, master);
        sink.on_pulse(target_, ev);
        next_strobe_fire_ms_ = now_ms + interval_ms;
    } else if (next_strobe_fire_ms_ == 0) {
        next_strobe_fire_ms_ = now_ms + interval_ms;
    }
    last_strobe_rate_ = rate;
}

}  // namespace dal
}  // namespace nocturnation
