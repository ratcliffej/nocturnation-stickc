// BeatPulseVisualisation - implementation (Epic 4.6 Block 8).
//
// Extracted from src/modes/director_mode.cpp. The Colour enum +
// helpers, the IBI/BPM tracking, the per-beat render fan-out, and the
// owned effects::Pulse all live here. DirectorMode shrinks to a
// shell that forwards events to the active vis.
//
// Wire output is byte-identical to the pre-migration code path. The three
// render targets fire in the same order (esp-now-broadcast, local
// DisplayClear, IR via Pulse), with the same envelope picker, same
// CHANCE_100, same RGB lookup.

#include "visualisations/beat_pulse.h"
#include "visualisations/visualisation_context.h"

#include "dal/dal.h"
#include "pixmob_protocol.h"

#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace visualisations {

using namespace nocturnation::dal;
using nocturnation::plugins::PluginKind;
using nocturnation::plugins::PowerProfile;
using nocturnation::plugins::PropertyBag;
using nocturnation::plugins::PropertyDef;
using nocturnation::plugins::PropertyType;
using nocturnation::plugins::PropertyValue;
using nocturnation::plugins::Span;

// =============================================================================
// Colour enum + helpers (verbatim from director_mode.cpp)
// =============================================================================
//
// Same six-state cycle including the OFF "skip IR" state that the
// pre-migration code used. The enum values are wire-stable (used as
// indices into kColourNames) - reordering would shift saved property
// bag values, so do not touch.

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
// BeatPulseVisualisation
// =============================================================================

hal::CapabilityMask BeatPulseVisualisation::required_capabilities() const {
    return hal::make_capability_mask(hal::Capability::Mic);
}

Span<const PropertyDef> BeatPulseVisualisation::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

PowerProfile BeatPulseVisualisation::power() const {
    PowerProfile p;
    p.needs_audio_frames   = true;
    p.needs_spectrum_frame = false;
    p.needs_8band_summary  = false;
    p.lcd_refresh_hz_max   = 20;
    p.tick_hz              = 0;
    return p;
}

void BeatPulseVisualisation::enter(VisualisationContext& ctx) {
    // Reset all per-show state. Property values come from the bag
    // (which falls back to the schema default if never set).
    last_beat_ms_  = 0;
    for (size_t i = 0; i < kIbiBufferSize; ++i) ibi_buffer_[i] = 0;
    ibi_index_     = 0;
    ibi_count_     = 0;
    estimated_bpm_ = 0.0f;

    pulse_.enter();
    sync_pulse_colour(ctx);
}

void BeatPulseVisualisation::exit(VisualisationContext& /*ctx*/) {
    pulse_.exit();
}

void BeatPulseVisualisation::on_audio_frame(VisualisationContext& ctx,
                                             const AudioFrameEvent& ev) {
    if (!ev.is_beat) return;

    const uint32_t now = millis();

    // BPM tracking: record IBI before updating last_beat_ms_. Constants
    // (300/1200 ms IBI bounds, kIbiBufferSize=8) preserved exactly from
    // the pre-migration implementation.
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
    // where we left off, but no render fan-out fires. Net effect is
    // identical to the pre-migration code which gated each render call
    // individually on !paused_.
    if (ctx.paused()) return;

    // Resolve the active colour from the property bag.
    const Colour colour =
        static_cast<Colour>(ctx.get_property("color").as_enum());

    // ---- Render fan-out, in the exact order the pre-migration code used.

    // 1. Wire to Lumes. Same envelope picker, same CHANCE_100.
    {
        uint8_t r=0, g=0, b=0;
        colour_to_rgb(colour, r, g, b);
        const effects::PulseEnvelope env = effects::envelope_for_bpm(estimated_bpm_);
        RgbPulseEvent wire{};
        wire.r = r; wire.g = g; wire.b = b;
        wire.attack  = env.attack;
        wire.sustain = env.sustain;
        wire.release = env.release;
        wire.chance  = pixmob::CHANCE_100;
        // "00:00" = broadcast to every class, every group. Epic 4.65 Block 7
        // migration from the legacy "esp-now-broadcast" device name; routes
        // to EspNowBroadcastDriver with target_class=0 / target_group=0 on
        // the LIGHT_PULSE payload, byte-identical to pre-migration wire
        // output for Lumes that pass the filter (everyone matches).
        DAL::render_fx("00:00", wire);
    }

    // 2. Screen flash. Preserved as DisplayClearEvent, not RgbPulseEvent.
    DAL::fire_display_clear("local",
        DisplayClearEvent{colour_screen_rgb(colour)});

    // 3. IR via PixMob driver. Pulse owns BPM->envelope; its on_beat
    //    fan-outs DAL::fire_rgb_pulse("all-pixmobs", ...). The OFF
    //    colour gates inside Pulse::fire (zero rgb -> early-return).
    pulse_.on_beat(now, estimated_bpm_);
}

void BeatPulseVisualisation::on_input_action(VisualisationContext& ctx,
                                              const hal::InputEvent& ev) {
    if (ev.action != hal::InputAction::Cycle) return;
    const uint8_t cur = ctx.get_property("color").as_enum();
    const uint8_t next = (cur + 1) % 6;
    ctx.set_property("color", PropertyValue::from_enum(next));
    sync_pulse_colour(ctx);
}

void BeatPulseVisualisation::sync_pulse_colour(VisualisationContext& ctx) {
    const Colour colour =
        static_cast<Colour>(ctx.get_property("color").as_enum());
    uint8_t r=0, g=0, b=0;
    colour_to_rgb(colour, r, g, b);
    pulse_.set_colour(r, g, b);
}

void BeatPulseVisualisation::on_property_changed(VisualisationContext& ctx,
                                                  const char* key) {
    // Settings-overlay edits route here. Re-sync the colour cached
    // inside effects::Pulse so the next beat fires the freshly-edited
    // colour rather than the value last read on Cycle / enter().
    // Cheap: just re-reads the bag and writes the three uint8s.
    if (key != nullptr && std::strcmp(key, "color") == 0) {
        sync_pulse_colour(ctx);
    }
}

void BeatPulseVisualisation::update_bpm_from_buffer() {
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
// Singletons
// =============================================================================

namespace {
BeatPulseVisualisation s_instance;
PropertyBag            s_bag(s_instance);
VisualisationContext   s_ctx(s_instance, s_bag);
}  // namespace

BeatPulseVisualisation* beat_pulse_instance()     { return &s_instance; }
PropertyBag&            beat_pulse_property_bag() { return s_bag; }
VisualisationContext&   beat_pulse_context()      { return s_ctx; }

VisualisationContext& BeatPulseVisualisation::context() {
    return s_ctx;
}

const char* beat_pulse_colour_label() {
    const Colour c =
        static_cast<Colour>(s_bag.get("color").as_enum());
    return colour_name(c);
}

uint16_t beat_pulse_colour_screen_rgb() {
    const Colour c =
        static_cast<Colour>(s_bag.get("color").as_enum());
    return colour_screen_rgb(c);
}

float beat_pulse_estimated_bpm() {
    return s_instance.estimated_bpm();
}

}  // namespace visualisations
}  // namespace nocturnation
