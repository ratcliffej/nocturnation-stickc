// SimpleBeatShow - implementation (Epic 4.7 Block 1).
//
// Preserves Epic 4.6 BeatPulse behaviour under the new Show plug-in
// framework. IBI/BPM tracking, Colour enum, and the per-beat fan-out
// match BeatPulseVisualisation; the flux meter tracking + full-screen
// rendering (previously chrome owned by DirectorMode) live
// here so the Show owns its canvas.
//
// Post-Epic-4.7 master-IR loopback (dispatch_output_class_group fires
// the master's ir-pixmob driver automatically when target_class is
// Light or wildcard and rgb is non-zero) means the Show no longer
// has to fire IR explicitly via effects::Pulse - one
// render_fx("00:00", ev) call reaches the wire AND the master's own
// IR LED. The Off colour gates inside the loopback (zero rgb skips IR)
// preserving the legacy "muted on Off" behaviour.

#include "shows/simple_beat_show.h"
#include "shows/show_context.h"

#include "dal/dal.h"
#include "effects/effects.h"          // PulseEnvelope + envelope_for_bpm
#include "pixmob_protocol.h"

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
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PowerProfile;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;

// =============================================================================
// Colour enum + helpers (verbatim from beat_pulse.cpp)
// =============================================================================
//
// Wire-stable enum values; reordering would shift saved property bag
// values across firmware versions.

namespace {

enum class Colour : uint8_t {
    Off = 0, Red, Green, Blue, Yellow, White
};

const char* colour_name(Colour c) {
    switch (c) {
        case Colour::Off:    return "OFF";
        case Colour::Red:    return "RED";
        case Colour::Green:  return "GREEN";
        case Colour::Blue:   return "BLUE";
        case Colour::Yellow: return "YELLOW";
        case Colour::White:  return "WHITE";
    }
    return "?";
}

uint16_t colour_screen_rgb(Colour c) {
    switch (c) {
        case Colour::Red:    return RED;
        case Colour::Green:  return GREEN;
        case Colour::Blue:   return BLUE;
        case Colour::Yellow: return YELLOW;
        case Colour::White:  return WHITE;
        case Colour::Off:    return BLACK;
    }
    return BLACK;
}

void colour_to_rgb(Colour c, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (c) {
        case Colour::Red:    r=0xFF; g=0x00; b=0x00; break;
        case Colour::Green:  r=0x00; g=0xFF; b=0x00; break;
        case Colour::Blue:   r=0x00; g=0x00; b=0xFF; break;
        case Colour::Yellow: r=0xFF; g=0xFF; b=0x00; break;
        case Colour::White:  r=0xFF; g=0xFF; b=0xFF; break;
        case Colour::Off:    r=0;    g=0;    b=0;    break;
    }
}

// Flux meter tuning - mirrors DirectorMode's pre-Block-1 values.
constexpr float kBeatMultiplier = 2.5f;
constexpr float kBaselineAlpha  = 0.02f;
constexpr float kVolumeGate     = 500.0f;

}  // namespace

// =============================================================================
// Property schema
// =============================================================================

namespace {

const char* const kColourNames[] = {
    "Off", "Red", "Green", "Blue", "Yellow", "White"
};

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"color",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(1),  // Red
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(5),
        /*display_name=*/"Primary Colour",
        /*unit=*/nullptr,
        /*enum_names=*/kColourNames,
    },
};

constexpr size_t kPropCount = sizeof(kProps) / sizeof(kProps[0]);

}  // namespace

// =============================================================================
// SimpleBeatShow
// =============================================================================

hal::CapabilityMask SimpleBeatShow::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Mic);
}

Span<const PropertyDef> SimpleBeatShow::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

PowerProfile SimpleBeatShow::power() const {
    PowerProfile p;
    p.needs_audio_frames   = true;
    p.needs_spectrum_frame = false;
    p.needs_8band_summary  = false;
    p.lcd_refresh_hz_max   = 20;
    p.tick_hz              = 0;
    return p;
}

void SimpleBeatShow::enter(ShowContext& /*ctx*/) {
    // Reset all per-show state. Property values come from the bag
    // (which falls back to the schema default if never set).
    last_beat_ms_      = 0;
    for (size_t i = 0; i < kIbiBufferSize; ++i) ibi_buffer_[i] = 0;
    ibi_index_         = 0;
    ibi_count_         = 0;
    estimated_bpm_     = 0.0f;
    baseline_flux_     = 100.0f;
    prev_bass_energy_  = 0.0f;
    current_flux_      = 0.0f;
    current_level_     = 0.0f;
}

void SimpleBeatShow::exit(ShowContext& /*ctx*/) {}

void SimpleBeatShow::on_audio_frame(ShowContext& /*ctx*/,
                                     const AudioFrameEvent& ev) {
    // Flux meter tracking - display-only. Beat firing routes through
    // on_beat_detected (the framework dispatches based on ev.is_beat).
    current_level_ = ev.overall_rms;

    if (current_level_ < kVolumeGate) {
        prev_bass_energy_ = 0.0f;
        return;
    }

    float flux = ev.bass_energy - prev_bass_energy_;
    if (flux < 0) flux = 0;
    prev_bass_energy_ = ev.bass_energy;
    current_flux_     = flux;

    baseline_flux_ = baseline_flux_ * (1.0f - kBaselineAlpha)
                   + flux * kBaselineAlpha;
}

void SimpleBeatShow::on_beat_detected(ShowContext& ctx, uint8_t /*strength*/) {
    const uint32_t now = millis();

    // BPM tracking: record IBI before updating last_beat_ms_. Constants
    // (300/1200 ms IBI bounds, kIbiBufferSize=8) preserved exactly from
    // the pre-Block-1 implementation.
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

    // Paused: BPM tracking still updates so the resume side picks up
    // where we left off, but no render fan-out fires. Identical to the
    // pre-Block-1 path.
    if (ctx.paused()) return;

    // Resolve the active colour from the property bag.
    const Colour colour =
        static_cast<Colour>(ctx.get_property("color").as_enum());

    // ---- Render fan-out.
    //
    // Single render_fx("00:00", ev) reaches both:
    //   - slaves via the esp-now-broadcast driver (target_class=0,
    //     target_group=0 = everyone everywhere)
    //   - master's own IR LED via the dispatch_output_class_group
    //     loopback that fires the ir-pixmob driver when target_class
    //     is 0 or 1 and rgb is non-zero (Off colour skips IR)
    //
    // Plus a separate fire_display_clear for the master-screen flash
    // (the screen uses DisplayClearEvent, not RgbPulseEvent).
    {
        uint8_t r=0, g=0, b=0;
        colour_to_rgb(colour, r, g, b);
        const effects::PulseEnvelope env = effects::envelope_for_bpm(estimated_bpm_);
        RgbPulseEvent ev{};
        ev.r = r; ev.g = g; ev.b = b;
        ev.attack  = env.attack;
        ev.sustain = env.sustain;
        ev.release = env.release;
        ev.chance  = pixmob::CHANCE_100;
        DAL::render_fx("00:00", ev);
    }

    DAL::fire_display_clear("local",
        DisplayClearEvent{colour_screen_rgb(colour)});

    (void)now;   // silence unused-variable warning if BPM tracking is the only consumer
}

void SimpleBeatShow::on_input_action(ShowContext& ctx,
                                      const hal::InputEvent& ev) {
    if (ev.action != hal::InputAction::Cycle) return;
    const uint8_t cur = ctx.get_property("color").as_enum();
    const uint8_t next = (cur + 1) % 6;
    ctx.set_property("color", PropertyValue::from_enum(next));
}

void SimpleBeatShow::update_bpm_from_buffer() {
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
//
// Owns the full canvas: status strip (show name), colour title,
// BPM, batt + IR fire counter, flux meter, footer hint. Mirrors the
// pre-Block-1 DirectorMode::draw() so the visible behaviour
// stays the same. The mode's draw cadence (50 ms) drives this; the
// beat-flash DisplayClear from on_beat_detected briefly overpaints
// before the next on_render() repaints the chrome.

void SimpleBeatShow::on_render(ShowContext& ctx) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});

    // Status strip: small show name at the very top.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 0, "Simple Beat", YELLOW, BLACK, 1});

    const Colour colour =
        static_cast<Colour>(ctx.get_property("color").as_enum());

    char title[32];
    std::snprintf(title, sizeof(title), " %s%s",
                  colour_name(colour),
                  ctx.paused() ? " : Muted" : "");
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 12, title, WHITE, BLACK, 3});

    char bpm[24];
    if (estimated_bpm_ > 0.0f) {
        std::snprintf(bpm, sizeof(bpm), " BPM: %.0f", estimated_bpm_);
    } else {
        std::snprintf(bpm, sizeof(bpm), " BPM: ---");
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 45, bpm, WHITE, BLACK, 2});

    // Batt + IR fire counter on one line. IR count is <=4 chars with k/M
    // suffix above 10000 so the line stays within 240 px at size 2.
    const uint32_t ir = DAL::driver_send_count("ir-pixmob");
    char ir_buf[8];
    if      (ir >= 1000000) std::snprintf(ir_buf, sizeof(ir_buf), "%luM",
                                          (unsigned long)(ir / 1000000));
    else if (ir >=   10000) std::snprintf(ir_buf, sizeof(ir_buf), "%luk",
                                          (unsigned long)(ir / 1000));
    else                    std::snprintf(ir_buf, sizeof(ir_buf), "%lu",
                                          (unsigned long)ir);
    char batt[40];
    std::snprintf(batt, sizeof(batt), "Batt: %d%% IR: %s",
                  DAL::battery_level("local"), ir_buf);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 75, batt, WHITE, BLACK, 2});

    // Flux meter via the BeatBarWidget. Equivalent to the pre-Block-2
    // inline rendering: bar fraction = ratio * 50 / 218 (clamped),
    // marker at kBeatMultiplier * 50 / 218 ~= 0.573. The 218 figure
    // is the inner drawable width (220 - 2 px frame).
    const int meterX = 10, meterY = 110, meterW = 220, meterH = 14;
    const float ratio = (baseline_flux_ > 1.0f)
                            ? current_flux_ / baseline_flux_
                            : 0.0f;
    constexpr float kBarScale       = 50.0f / 218.0f;
    constexpr float kMarkerFraction = kBeatMultiplier * 50.0f / 218.0f;
    flux_bar_.update(ratio * kBarScale, kMarkerFraction);
    flux_bar_.draw(meterX, meterY, meterW, meterH);

    // Operator hint footer.
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        4, 128, "B: cycle  A-hold: set  B-hold: pick",
        WHITE, BLACK, 1});
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
SimpleBeatShow s_instance;
PropertyBag    s_bag(s_instance);
ShowContext    s_ctx(s_instance, s_bag);
}  // namespace

SimpleBeatShow*       simple_beat_show_instance()     { return &s_instance; }
PropertyBag&          simple_beat_show_property_bag() { return s_bag; }
ShowContext&          simple_beat_show_context()      { return s_ctx; }

ShowContext& SimpleBeatShow::context() {
    return s_ctx;
}

}  // namespace shows
}  // namespace nocturnation
