// SpectrumBarsVisualisation - implementation (Epic 4.6 Block 11).
//
// First vis to consume SpectrumFrameEvent, verifying Block 7's pipeline
// gate end-to-end. Render is 32 bars across the 240 px master LCD,
// heights driven by ev.magnitudes scaled by the "sensitivity" property,
// and the "band_focus" property tints the focused range so the operator
// can spot Bass/Mid/Treble onset at a glance. Confirm fires a manual
// guaranteed pulse - the sound-check tool the brief asks for.

#include "visualisations/spectrum_bars.h"
#include "visualisations/visualisation_context.h"

#include "dal/dal.h"
#include "pixmob_protocol.h"

#include <cmath>

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
// Property schema
// =============================================================================

namespace {

// Band-focus enum value names. Index matches the persisted U8 value.
const char* const kBandFocusNames[] = {
    "All", "Bass", "Mid", "Treble"
};

const PropertyDef kProps[] = {
    PropertyDef{
        /*key=*/"band_focus",
        /*type=*/PropertyType::Enum,
        /*default_value=*/PropertyValue::from_enum(0),  // All
        /*min_value=*/    PropertyValue::from_enum(0),
        /*max_value=*/    PropertyValue::from_enum(3),
        /*display_name=*/"Band Focus",
        /*unit=*/nullptr,
        /*enum_names=*/kBandFocusNames,
    },
    PropertyDef{
        /*key=*/"sensitivity",
        /*type=*/PropertyType::U8,
        /*default_value=*/PropertyValue::from_u8(5),
        /*min_value=*/    PropertyValue::from_u8(1),
        /*max_value=*/    PropertyValue::from_u8(10),
        /*display_name=*/"Sensitivity",
        /*unit=*/nullptr,
        /*enum_names=*/nullptr,
    },
};

constexpr size_t kPropCount = sizeof(kProps) / sizeof(kProps[0]);

// LCD geometry. Bars draw from y=14 down to y=134 (120 px tall) on the
// StickC Plus2's 240x135 panel. 32 bars * (6 px wide + 1 px gap) = 224
// px, centred with an 8 px margin each side. The previous 7+1 sizing
// produced 32*8 = 256 px which overflowed the panel by 16 px and clipped
// the two highest-frequency bands off the right edge.
constexpr int kBarsTopY      = 14;
constexpr int kBarsBottomY   = 134;
constexpr int kBarsMaxHeight = kBarsBottomY - kBarsTopY;
constexpr int kBarWidth      = 6;
constexpr int kBarGap        = 1;
constexpr int kBarsLeftX     = 8;

// Magnitude-to-bar-height calibration. compute_spectrum_frame() in
// audio_analyser.cpp accumulates RAW linear FFT magnitudes per band -
// observed range on Plus2 hardware spans nearly five orders of
// magnitude: silence median ~1500 / max ~12000, normal music median
// ~5000-10000 / max ~50000-150000, peak drops max ~400000+. Linear
// scaling can't represent that span; we log-compress first, then
// subtract a floor in log space and scale by sensitivity.
//
// Numbers below are tuned against captured serial output (1 Hz
// [SPEC] dump in draw_spectrum's diagnostic block):
//   log2(1500)   ≈ 10.5  (silence floor)
//   log2(10000)  ≈ 13.3  (quiet music median)
//   log2(150000) ≈ 17.2  (loud music max)
//   log2(400000) ≈ 18.6  (peak drop)
//
// kMagFloorLog2 sits at 10.0 so silence shows no bars and quiet music
// shows small bars; kSensScale = 0.025 maps a log2 span of 8 at
// sens=5 to full bar height (so anything above log2 ~18 / mag ~262k
// clamps to full).
constexpr float kMagFloorLog2 = 10.0f;
constexpr float kSensScale    = 0.025f;

// Band-focus group bounds (inclusive). Bass = bands 0..9 (lowest 10 of
// 32 log-spaced bins, sub-200-ish-Hz on the StickC analyser), Mid =
// 10..21, Treble = 22..31. These are visual focus regions, not the
// analyser's own boundary definitions.
constexpr size_t kBassEnd   = 10;        // exclusive
constexpr size_t kMidEnd    = 22;        // exclusive
// Treble is [kMidEnd .. kBands).

}  // namespace

// =============================================================================
// SpectrumBarsVisualisation
// =============================================================================

hal::CapabilityMask SpectrumBarsVisualisation::required_capabilities() const {
    // Mic is required to source any audio frames at all. The picker
    // gate will further hide this vis on hosts without
    // AnalyserSpectrumFrame because DAL composes SpectrumFrame as an
    // input only when the host's HAL declares that capability - a vis
    // declaring needs_spectrum_frame on a host without the analyser
    // simply never receives spectrum events (and falls back to a
    // beat-only render).
    return hal::make_capability_mask(hal::Capability::Mic);
}

Span<const PropertyDef> SpectrumBarsVisualisation::properties() const {
    return Span<const PropertyDef>{kProps, kPropCount};
}

PowerProfile SpectrumBarsVisualisation::power() const {
    PowerProfile p;
    p.needs_audio_frames    = true;
    p.needs_spectrum_frame  = true;     // load-bearing for Block 7's gate
    p.needs_8band_summary   = false;
    p.lcd_refresh_hz_max    = 30;
    p.tick_hz               = 0;
    return p;
}

void SpectrumBarsVisualisation::enter(VisualisationContext& /*ctx*/) {
    last_draw_ms_ = 0;
    // Clear residue from whatever screen owner ran before us (picker
    // overlay text, BeatPulse pulse rect, ...). draw_spectrum only
    // clears the bars region (y=14..134), so y=0..13 would otherwise
    // keep the picker's "Visualisation" title visible until the next
    // overlay open.
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
}

void SpectrumBarsVisualisation::exit(VisualisationContext& /*ctx*/) {}

void SpectrumBarsVisualisation::on_audio_frame(VisualisationContext& /*ctx*/,
                                                const AudioFrameEvent& /*ev*/) {
    // SpectrumBars is spectrum-driven; the audio-frame hook is here to
    // declare needs_audio_frames in the power profile so beats also
    // reach the vis (a future tweak could use is_beat to drive an
    // accent overlay on top of the bars). For Block 11 it's a no-op.
}

void SpectrumBarsVisualisation::on_spectrum_frame(VisualisationContext& ctx,
                                                   const SpectrumFrameEvent& ev) {
    if (ctx.paused()) return;
    draw_spectrum(ctx, ev);
}

void SpectrumBarsVisualisation::on_input_action(VisualisationContext& ctx,
                                                 const hal::InputEvent& ev) {
    // Confirm = manual sound-check fire. Cycle / CyclePrev / etc. are
    // intentionally ignored - band_focus / sensitivity are edited via
    // the auto-generated Settings overlay, not by raw input actions on
    // the main vis screen.
    if (ev.action != hal::InputAction::Confirm) return;
    if (ctx.paused()) return;
    fire_manual_beat(ctx);
}

void SpectrumBarsVisualisation::draw_spectrum(VisualisationContext& ctx,
                                               const SpectrumFrameEvent& ev) {
    // Throttle redraws by power().lcd_refresh_hz_max (30 Hz -> ~33 ms
    // minimum between frames). The analyser may push spectrum events
    // faster than the LCD can repaint cleanly; spillover is dropped.
    const uint32_t now = ctx.now_ms();
    if (last_draw_ms_ != 0 && (now - last_draw_ms_) < 33u) return;
    last_draw_ms_ = now;

#ifdef ARDUINO
    // Diagnostic: dump min/max/median band magnitude once per second so
    // the calibration constants can be set against observed hardware
    // values rather than guessed. Drop this block once kMagFloor /
    // kSensScale are dialled in.
    {
        static uint32_t last_log_ms = 0;
        if (now - last_log_ms >= 1000u) {
            last_log_ms = now;
            float mn = ev.magnitudes[0], mx = ev.magnitudes[0];
            float sorted[SpectrumFrameEvent::kBands];
            for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
                const float m = ev.magnitudes[i];
                if (m < mn) mn = m;
                if (m > mx) mx = m;
                sorted[i] = m;
            }
            for (size_t i = 1; i < SpectrumFrameEvent::kBands; ++i) {
                float k = sorted[i]; size_t j = i;
                while (j > 0 && sorted[j-1] > k) { sorted[j] = sorted[j-1]; --j; }
                sorted[j] = k;
            }
            const float med = sorted[SpectrumFrameEvent::kBands / 2];
            Serial.printf("[SPEC] min=%.1f med=%.1f max=%.1f\n", mn, med, mx);
        }
    }
#endif

    const uint8_t  band_focus  = ctx.get_property("band_focus").as_enum();
    const uint8_t  sensitivity = ctx.get_property("sensitivity").as_u8();
    const uint16_t focus_tint  = focused_tint(band_focus);
    const float    sens_scale  = static_cast<float>(sensitivity);

    // Clear bars region only (preserve status strip above).
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        0, kBarsTopY, 240, kBarsMaxHeight, BLACK});

    for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
        // Log-compress first (the analyser ships raw linear magnitudes
        // spanning ~5 orders of magnitude), then subtract the silence
        // floor in log space and scale by sensitivity. See
        // kMagFloorLog2 / kSensScale notes above for the calibration.
        // log2(1+m) handles m=0 cleanly (yields 0).
        const float log_mag = std::log2(1.0f + ev.magnitudes[i]);
        float v = (log_mag - kMagFloorLog2) * sens_scale * kSensScale;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        const int h = static_cast<int>(v * static_cast<float>(kBarsMaxHeight));
        if (h <= 0) continue;

        const int x = kBarsLeftX + static_cast<int>(i) * (kBarWidth + kBarGap);
        const int y = kBarsBottomY - h;

        const uint16_t tint = band_is_focused(band_focus, i) ? focus_tint
                                                              : WHITE;
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            x, y, kBarWidth, h, tint});
    }
}

void SpectrumBarsVisualisation::fire_manual_beat(VisualisationContext& ctx) {
    const uint8_t band_focus = ctx.get_property("band_focus").as_enum();
    uint8_t r = 0, g = 0, b = 0;
    focused_rgb(band_focus, r, g, b);

    // Punchy default envelope; CHANCE_100 because the operator wants a
    // guaranteed fire for IR validation - no probability gating.
    RgbPulseEvent pulse{};
    pulse.r       = r;
    pulse.g       = g;
    pulse.b       = b;
    pulse.attack  = pixmob::T_32_MS;
    pulse.sustain = pixmob::T_96_MS;
    pulse.release = pixmob::T_96_MS;
    pulse.chance  = pixmob::CHANCE_100;

    // Three-target fan-out mirroring BeatPulse's per-beat shape, in the
    // same order: wire -> screen flash -> IR via render_fx.
    DAL::render_fx("esp-now-broadcast", pulse);
    DAL::fire_display_clear("local",
        DisplayClearEvent{focused_tint(band_focus)});
    DAL::render_fx("all-pixmobs", pulse);
}

bool SpectrumBarsVisualisation::band_is_focused(uint8_t band_focus, size_t i) {
    switch (band_focus) {
        case 0: return true;                                     // All
        case 1: return i < kBassEnd;                              // Bass
        case 2: return i >= kBassEnd && i < kMidEnd;              // Mid
        case 3: return i >= kMidEnd;                              // Treble
    }
    return true;
}

uint16_t SpectrumBarsVisualisation::focused_tint(uint8_t band_focus) {
    switch (band_focus) {
        case 0: return WHITE;       // All - no highlight, neutral bars
        case 1: return RED;         // Bass
        case 2: return GREEN;       // Mid
        case 3: return BLUE;        // Treble
    }
    return WHITE;
}

void SpectrumBarsVisualisation::focused_rgb(uint8_t band_focus,
                                             uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (band_focus) {
        case 1: r = 0xFF; g = 0x00; b = 0x00; return;   // Bass red
        case 2: r = 0x00; g = 0xFF; b = 0x00; return;   // Mid green
        case 3: r = 0x00; g = 0x00; b = 0xFF; return;   // Treble blue
        case 0:
        default: r = 0xFF; g = 0xFF; b = 0xFF; return;  // All white
    }
}

// =============================================================================
// Singletons
// =============================================================================

namespace {
SpectrumBarsVisualisation s_instance;
PropertyBag               s_bag(s_instance);
VisualisationContext      s_ctx(s_instance, s_bag);
}  // namespace

SpectrumBarsVisualisation* spectrum_bars_instance()     { return &s_instance; }
PropertyBag&               spectrum_bars_property_bag() { return s_bag; }
VisualisationContext&      spectrum_bars_context()      { return s_ctx; }

VisualisationContext& SpectrumBarsVisualisation::context() {
    return s_ctx;
}

}  // namespace visualisations
}  // namespace nocturnation
