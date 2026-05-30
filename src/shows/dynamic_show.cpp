// DynamicShow implementation (Epic 4.7 Block 5).
//
// Headline Show: consumes the Block 3 multi-band onset / descriptor
// hooks + Block 4 section state, routes effects to class+group
// targets per Epic 4.65, and renders a diagnostic screen showing
// current section + descriptor readouts + a live spectrum widget.

#include "shows/dynamic_show.h"
#include "shows/show_context.h"

#include "dal/dal.h"
#include "dal/analyser/section_detector.h"
#include "effects/effects.h"
#include "pulse/envelope.h"           // protocol-neutral Time / Chance

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace shows {

using namespace nocturnation::dal;
using nocturnation::plugins::PowerProfile;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;

namespace {

// =============================================================================
// Property schema
// =============================================================================
//
// `groups` controls how kick / snare / hi-hat distribute across PixMob
// bracelet groups. Default 1 broadcasts everything to group 0 so a
// fresh bench deployment with un-grouped bracelets responds out of
// the box; the operator bumps to 3 once bracelets are programmed
// into groups 1 / 2 / 3 for full per-drum separation.

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"groups",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(1),
        /*min_value=*/    PropertyValue::from_u8(1),
        /*max_value=*/    PropertyValue::from_u8(3),
        /*display_name=*/"Groups",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
};

constexpr size_t kPropCount = sizeof(kProps) / sizeof(kProps[0]);

// Map (drum, group_count) to a render_fx target string. Static
// constants so we don't construct strings every fire.
const char* kick_target_for(uint8_t group_count) {
    if (group_count >= 2) return "01:01";
    return "00:00";
}
const char* snare_target_for(uint8_t group_count) {
    if (group_count >= 2) return "01:02";
    return "00:00";
}
const char* hihat_target_for(uint8_t group_count) {
    if (group_count >= 3) return "01:03";
    if (group_count >= 2) return "01:02";
    return "00:00";
}

// =============================================================================
// Colour math
// =============================================================================
//
// Centroid (0..255) maps to hue across the cool-to-warm sweep:
//   0   -> 240  blue
//   128 -> 120  green
//   255 -> 0    red
//
// Section adjusts the hue offset + saturation / brightness. DROP
// short-circuits to white at full brightness.

inline float clampf(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

void hsv_to_rgb(float h, float s, float v,
                 uint8_t& r, uint8_t& g, uint8_t& b) {
    // Standard HSV -> RGB. h in [0, 360), s/v in [0, 1].
    while (h < 0.0f)    h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    const float c = v * s;
    const float x = c * (1.0f - std::fabs(std::fmod(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float r1, g1, b1;
    if      (h <  60.0f) { r1 = c; g1 = x; b1 = 0; }
    else if (h < 120.0f) { r1 = x; g1 = c; b1 = 0; }
    else if (h < 180.0f) { r1 = 0; g1 = c; b1 = x; }
    else if (h < 240.0f) { r1 = 0; g1 = x; b1 = c; }
    else if (h < 300.0f) { r1 = x; g1 = 0; b1 = c; }
    else                 { r1 = c; g1 = 0; b1 = x; }
    r = static_cast<uint8_t>(clampf((r1 + m) * 255.0f, 0.0f, 255.0f));
    g = static_cast<uint8_t>(clampf((g1 + m) * 255.0f, 0.0f, 255.0f));
    b = static_cast<uint8_t>(clampf((b1 + m) * 255.0f, 0.0f, 255.0f));
}

void compute_colour(uint8_t centroid,
                     uint8_t energy,
                     uint8_t section,
                     uint8_t& out_r,
                     uint8_t& out_g,
                     uint8_t& out_b) {
    using S = analyser::SectionType;
    if (section == static_cast<uint8_t>(S::Drop)) {
        out_r = out_g = out_b = 255;     // white drop
        return;
    }

    // Base hue 240 -> 0 as centroid rises.
    float hue = 240.0f - (static_cast<float>(centroid) * 240.0f / 255.0f);

    // Section adjustments.
    float sat = 1.0f;
    float val = static_cast<float>(energy) / 255.0f;
    switch (static_cast<S>(section)) {
        case S::Verse:
            hue += 20.0f;                // push cooler
            val = clampf(val, 0.0f, 0.75f);
            break;
        case S::Chorus:
            // Natural - centroid-driven hue at full saturation/value.
            break;
        case S::BuildUp:
            hue -= 20.0f;                // push warmer (anticipation)
            break;
        case S::Breakdown:
            val *= 0.25f;                // dim
            sat *= 0.5f;                 // wash out
            break;
        case S::Drop:                    // handled above
        case S::Unknown:
        default:
            break;
    }

    // Minimum value floor so silence doesn't fire pure black (visible
    // pulses help operators see that events are firing).
    if (val < 0.10f) val = 0.10f;

    hsv_to_rgb(hue, sat, val, out_r, out_g, out_b);
}

pulse::Chance density_to_chance(uint8_t density) {
    if (density >= 200) return pulse::CHANCE_100;
    if (density >= 150) return pulse::CHANCE_88;
    if (density >= 100) return pulse::CHANCE_67;
    if (density >=  60) return pulse::CHANCE_50;
    if (density >=  30) return pulse::CHANCE_32;
    if (density >=  10) return pulse::CHANCE_16;
    return pulse::CHANCE_10;
}

const char* section_label(uint8_t s) {
    using S = analyser::SectionType;
    switch (static_cast<S>(s)) {
        case S::Verse:             return "VERSE";
        case S::Chorus:            return "CHORUS";
        case S::BuildUp:           return "BUILDUP";
        case S::Breakdown:         return "BREAKDN";
        case S::VocalsOnly:        return "VOCALS";
        case S::InstrumentalBreak: return "INSTR";
        case S::Drop:              return "DROP";
        case S::Unknown:
        default:                   return "...";
    }
}

uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(
        ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

}  // namespace

// =============================================================================
// DynamicShow
// =============================================================================

hal::CapabilityMask DynamicShow::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Mic);
}

Span<const PropertyDef> DynamicShow::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

PowerProfile DynamicShow::power() const {
    PowerProfile p;
    p.needs_audio_frames   = true;
    p.needs_spectrum_frame = false;
    p.needs_8band_summary  = true;
    p.lcd_refresh_hz_max   = 20;
    p.tick_hz              = 0;
    return p;
}

void DynamicShow::enter(ShowContext& /*ctx*/) {
    centroid_              = 128;
    energy_                = 128;
    density_               = 0;
    section_               = 0;
    last_beat_ms_          = 0;
    for (size_t i = 0; i < kIbiBufferSize; ++i) ibi_buffer_[i] = 0;
    ibi_index_             = 0;
    ibi_count_             = 0;
    estimated_bpm_         = 0.0f;
    for (size_t i = 0; i < 7; ++i) band_values_[i] = 0.0f;
}

void DynamicShow::exit(ShowContext& /*ctx*/) {}

void DynamicShow::on_audio_frame(ShowContext& /*ctx*/,
                                  const AudioFrameEvent& ev) {
    // Cache 7 perceptual bands for the on-screen spectrum widget.
    // Same log-normalisation as ConfigMode > Level Tuning, with the
    // SpectrumBars widget's default sensitivity 5.
    constexpr float kMagFloorLog2 = 13.0f;
    constexpr float kSensScale    = 0.025f;
    constexpr float kSens         = 5.0f;
    const float bands[7] = {
        ev.sub_bass, ev.bass, ev.low_mids, ev.midrange,
        ev.high_mids, ev.presence, ev.air
    };
    for (size_t i = 0; i < 7; ++i) {
        const float sum = bands[i];
        if (sum <= 0.0f) {
            band_values_[i] = 0.0f;
        } else {
            const float log_mag = std::log2(1.0f + sum);
            float v = (log_mag - kMagFloorLog2) * kSens * kSensScale;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            band_values_[i] = v;
        }
    }
}

void DynamicShow::on_beat_detected(ShowContext& ctx, uint8_t strength) {
    // BPM tracking (kick band only - the most reliable BPM signal).
    const uint32_t now = millis();
    if (last_beat_ms_ > 0) {
        const uint32_t ibi = now - last_beat_ms_;
        if (ibi >= 300 && ibi <= 1200) {
            ibi_buffer_[ibi_index_] = ibi;
            ibi_index_ = (ibi_index_ + 1) % kIbiBufferSize;
            if (ibi_count_ < kIbiBufferSize) ibi_count_++;
            update_bpm_from_buffer();
        }
    }
    last_beat_ms_ = now;

    if (ctx.paused()) return;

    const uint8_t group_count = ctx.get_property("groups").as_u8();
    fire_event(ctx, kick_target_for(group_count), strength);

    // Director local screen flash on kick so the operator sees the show
    // is alive even without bracelets. Colour from the same compute
    // that drives the wire output.
    uint8_t r=0, g=0, b=0;
    compute_colour(centroid_, energy_, section_, r, g, b);
    DAL::fire_display_clear("local",
        DisplayClearEvent{rgb888_to_rgb565(r, g, b)});
}

void DynamicShow::on_snare_detected(ShowContext& ctx, uint8_t strength) {
    if (ctx.paused()) return;
    const uint8_t group_count = ctx.get_property("groups").as_u8();
    fire_event(ctx, snare_target_for(group_count), strength);
}

void DynamicShow::on_hihat_detected(ShowContext& ctx, uint8_t strength) {
    if (ctx.paused()) return;
    const uint8_t group_count = ctx.get_property("groups").as_u8();
    fire_event(ctx, hihat_target_for(group_count), strength);
}

void DynamicShow::on_music_descriptor(ShowContext& /*ctx*/,
                                       uint8_t centroid,
                                       uint8_t energy,
                                       uint8_t density) {
    centroid_ = centroid;
    energy_   = energy;
    density_  = density;
}

void DynamicShow::on_section_change(ShowContext& /*ctx*/, uint8_t section) {
    section_ = section;
}

void DynamicShow::fire_event(ShowContext& /*ctx*/,
                              const char* target,
                              uint8_t /*strength*/) {
    uint8_t r=0, g=0, b=0;
    compute_colour(centroid_, energy_, section_, r, g, b);

    const effects::PulseEnvelope env =
        effects::envelope_for_bpm(estimated_bpm_);

    RgbPulseEvent ev{};
    ev.r = r; ev.g = g; ev.b = b;
    ev.attack  = env.attack;
    ev.sustain = env.sustain;
    ev.release = env.release;
    ev.chance  = density_to_chance(density_);
    DAL::render_fx(target, ev);
}

void DynamicShow::update_bpm_from_buffer() {
    if (ibi_count_ < 3) return;
    uint32_t sorted[kIbiBufferSize];
    for (size_t i = 0; i < ibi_count_; ++i) sorted[i] = ibi_buffer_[i];
    for (size_t i = 1; i < ibi_count_; ++i) {
        uint32_t key = sorted[i];
        size_t j = i;
        while (j > 0 && sorted[j-1] > key) { sorted[j] = sorted[j-1]; --j; }
        sorted[j] = key;
    }
    const uint32_t med = (ibi_count_ % 2 == 1)
        ? sorted[ibi_count_ / 2]
        : (sorted[ibi_count_ / 2 - 1] + sorted[ibi_count_ / 2]) / 2;
    if (med > 50) estimated_bpm_ = 60000.0f / (float)med;
}

// =============================================================================
// Screen rendering
// =============================================================================

void DynamicShow::on_render(ShowContext& /*ctx*/) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    // Title strip.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 0, "Dynamic Show", YELLOW, BLACK, 1});

    // Section label (size 3, centred-ish).
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 12, section_label(section_), WHITE, BLACK, 3});

    // Descriptor readout.
    char descr[40];
    std::snprintf(descr, sizeof(descr), "C:%3u E:%3u D:%3u  BPM:%.0f",
                  static_cast<unsigned>(centroid_),
                  static_cast<unsigned>(energy_),
                  static_cast<unsigned>(density_),
                  estimated_bpm_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 45, descr, WHITE, BLACK, 1});

    // Spectrum widget filling the bottom region.
    spectrum_widget_.update(band_values_);
    spectrum_widget_.draw(0, 60, 240, 60);

    // Footer hint.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        4, 128, "B: cycle  A-hold: set  B-hold: pick",
        WHITE, BLACK, 1});
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
DynamicShow s_instance;
PropertyBag s_bag(s_instance);
ShowContext s_ctx(s_instance, s_bag);
}  // namespace

DynamicShow*  dynamic_show_instance()     { return &s_instance; }
PropertyBag&  dynamic_show_property_bag() { return s_bag; }
ShowContext&  dynamic_show_context()      { return s_ctx; }

ShowContext& DynamicShow::context() {
    return s_ctx;
}

}  // namespace shows
}  // namespace nocturnation
