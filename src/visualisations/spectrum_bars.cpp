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

// =============================================================================
// Perceptual band layout (7 bands, per Audible Genius music-production
// reference - see PerceptualBoundsHz in band_layout.h). Mud (0-20 Hz) is
// merged into Sub Bass because the spectrum surface starts at 30 Hz; Mud
// bins are not present in the spectrum frame anyway.
// =============================================================================

enum class Perceptual : uint8_t {
    SubBass = 0,    // 20-60 Hz (incl Mud below 20 Hz)
    Bass,           // 60-250 Hz
    LowMids,        // 250-500 Hz
    Midrange,       // 500 Hz - 2 kHz
    HighMids,       // 2 kHz - 4 kHz
    Presence,       // 4 kHz - 6 kHz
    Air,            // 6 kHz - 20 kHz
};

constexpr size_t kPerceptualCount = 7;

// Hard-coded spectrum-band -> perceptual-band mapping for the canonical
// 16 kHz / 32 log-spaced bands operating point. Spectrum band i covers
// roughly [30 * 1.193^i, 30 * 1.193^(i+1)) Hz; the centre is used for
// bucket assignment against PerceptualBoundsHz. Indices below worked
// out from those centres:
//   bands  0..3   -> Sub Bass   (centres ~33..55 Hz)
//   bands  4..11  -> Bass       (centres ~66..227 Hz)
//   bands 12..15  -> Low Mids   (centres ~271..460 Hz)
//   bands 16..23  -> Midrange   (centres ~548..1884 Hz)
//   bands 24..27  -> High Mids  (centres ~2247..3812 Hz)
//   bands 28..29  -> Presence   (centres ~4546..5422 Hz)
//   bands 30..31  -> Air        (centres ~6467..7713 Hz)
//
// Sum is 4+8+4+8+4+2+2 = 32. If the operating point ever changes
// (future 48 kHz / 2048 FFT) this table will need to be recomputed or
// generated dynamically from sample rate; flagged in a comment near
// PowerProfile.
constexpr Perceptual kBandMap[32] = {
    Perceptual::SubBass, Perceptual::SubBass, Perceptual::SubBass, Perceptual::SubBass,
    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,
    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,    Perceptual::Bass,
    Perceptual::LowMids, Perceptual::LowMids, Perceptual::LowMids, Perceptual::LowMids,
    Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,
    Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,Perceptual::Midrange,
    Perceptual::HighMids,Perceptual::HighMids,Perceptual::HighMids,Perceptual::HighMids,
    Perceptual::Presence,Perceptual::Presence,
    Perceptual::Air,     Perceptual::Air,
};

// Short labels (3-4 chars) under each bar. Index by Perceptual.
const char* const kPerceptualLabels[kPerceptualCount] = {
    "Sub", "Bass", "Lows", "Mid", "Hi", "Pres", "Air"
};

// RGB565 colour per band - warm-to-cool rainbow mapping low frequencies
// to warm and high frequencies to cool. Indices match Perceptual enum.
//   Sub Bass: deep purple
//   Bass:     red
//   Low Mids: orange
//   Midrange: yellow
//   High Mids:green
//   Presence: cyan
//   Air:      light blue
constexpr uint16_t kPerceptualColours[kPerceptualCount] = {
    0x801F,   // Sub Bass  - deep purple
    0xF800,   // Bass      - red
    0xFB60,   // Low Mids  - orange
    0xFFE0,   // Midrange  - yellow
    0x07E0,   // High Mids - green
    0x07FF,   // Presence  - cyan
    0x051F,   // Air       - light blue
};

// =============================================================================
// Property schema. Only sensitivity remains - band_focus was dropped
// when the vis moved from 32 raw bars to 7 fixed perceptual bands, each
// with its own permanent colour. Manual fire on Confirm uses white
// (operator's sound-check tool; tinting it per-band added complexity
// without earning anything once the bars are labelled).
// =============================================================================

const PropertyDef kProps[] = {
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

// LCD geometry. 7 bars at 30 px wide + 4 px gap = 7*30 + 6*4 = 234 px,
// centred on the 240 px panel with 3 px margin each side. Labels render
// in a 14 px strip below the bars (y=120..134) at size 1.
constexpr int kBarsTopY        = 14;
constexpr int kBarsBottomY     = 120;    // labels live below this line
constexpr int kBarsMaxHeight   = kBarsBottomY - kBarsTopY;
constexpr int kLabelY          = 124;
constexpr int kBarWidth        = 30;
constexpr int kBarGap          = 4;
constexpr int kBarsLeftX       = 3;

// Magnitude-to-bar-height calibration. compute_spectrum_frame() ships
// RAW linear FFT magnitudes per band; perceptual aggregates sum 2-8 of
// those raw bands together, so values are ~2-8x larger than individual
// spectrum bands. Observed perceptual-band ranges (derived from the
// 1 Hz [SPEC] dump at the per-band level):
//   silence:     ~5k..15k    (log2 ~12.3..13.9)
//   quiet music: ~20k..80k   (log2 ~14.3..16.3)
//   loud music:  ~100k..500k (log2 ~16.6..18.9)
//   peak drop:   up to 2M    (log2 ~21.0)
//
// Log-compress, subtract floor at log2=13.0 (silence cutoff), scale by
// sens * 0.025 so default sens=5 maps a log2 span of 8 to full bar
// (anything above log2 ~21 / mag ~2M clamps full).
constexpr float kMagFloorLog2 = 13.0f;
constexpr float kSensScale    = 0.025f;

// White RGB for the Confirm manual fire. CHANCE_100 in fire_manual_beat
// keeps it a guaranteed sound-check pulse (no probability gating).
constexpr uint8_t kFireR = 0xFF;
constexpr uint8_t kFireG = 0xFF;
constexpr uint8_t kFireB = 0xFF;

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

    // Roll up 32 log-spaced spectrum bands into 7 perceptual bands per
    // kBandMap. Sums match what compute_band_summaries in
    // audio_analyser.cpp would produce for these frequency ranges,
    // approximated against the already-summed spectrum data (the vis
    // doesn't have access to the raw FFT magnitudes).
    float band_sums[kPerceptualCount] = {0, 0, 0, 0, 0, 0, 0};
    for (size_t i = 0; i < SpectrumFrameEvent::kBands; ++i) {
        band_sums[static_cast<size_t>(kBandMap[i])] += ev.magnitudes[i];
    }

#ifdef ARDUINO
    // Diagnostic: dump the 7 perceptual-band aggregates once per second
    // so the floor / scale can be re-tuned against observed hardware
    // values. Drop this block once the calibration is dialled in.
    {
        static uint32_t last_log_ms = 0;
        if (now - last_log_ms >= 1000u) {
            last_log_ms = now;
            Serial.printf("[SPEC7] Sub=%.0f Bass=%.0f Low=%.0f Mid=%.0f "
                          "Hi=%.0f Pres=%.0f Air=%.0f\n",
                          band_sums[0], band_sums[1], band_sums[2], band_sums[3],
                          band_sums[4], band_sums[5], band_sums[6]);
        }
    }
#endif

    const uint8_t sensitivity = ctx.get_property("sensitivity").as_u8();
    const float   sens_scale  = static_cast<float>(sensitivity);

    // Clear the bars + labels region (preserve any status strip above).
    DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
        0, kBarsTopY, 240, 135 - kBarsTopY, BLACK});

    for (size_t b = 0; b < kPerceptualCount; ++b) {
        // Log-compress the perceptual aggregate (raw linear values
        // span ~5 orders of magnitude). log2(1+m) handles m=0 cleanly.
        const float log_mag = std::log2(1.0f + band_sums[b]);
        float v = (log_mag - kMagFloorLog2) * sens_scale * kSensScale;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;

        const int x = kBarsLeftX + static_cast<int>(b) * (kBarWidth + kBarGap);
        const int h = static_cast<int>(v * static_cast<float>(kBarsMaxHeight));

        // Always draw a 2 px floor under each bar so the colour bands
        // are visible even on silence - acts as a legend / sanity check
        // that the vis is alive.
        const int floor_h  = 2;
        const int floor_y  = kBarsBottomY - floor_h;
        DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
            x, floor_y, kBarWidth, floor_h, kPerceptualColours[b]});

        if (h > floor_h) {
            const int y = kBarsBottomY - h;
            DAL::fire_display_fill_rect("local", DisplayFillRectEvent{
                x, y, kBarWidth, h - floor_h, kPerceptualColours[b]});
        }

        // Label under each bar. Centre approximately by eyeballing -
        // 30 px wide bar / ~6 px per size-1 char gives room for ~4
        // chars; kPerceptualLabels are all 3-4 chars so they fit.
        const int label_x = x + 4;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            label_x, kLabelY, kPerceptualLabels[b],
            kPerceptualColours[b], BLACK, 1});
    }
}

void SpectrumBarsVisualisation::fire_manual_beat(VisualisationContext& /*ctx*/) {
    // Sound-check tool: a guaranteed white pulse across the three
    // render targets at CHANCE_100. White was chosen over per-band
    // tinting because the labelled bars already communicate band
    // identity; tinting the fire colour to match a non-existent
    // "selected band" added complexity without earning anything.
    RgbPulseEvent pulse{};
    pulse.r       = kFireR;
    pulse.g       = kFireG;
    pulse.b       = kFireB;
    pulse.attack  = pixmob::T_32_MS;
    pulse.sustain = pixmob::T_96_MS;
    pulse.release = pixmob::T_96_MS;
    pulse.chance  = pixmob::CHANCE_100;

    // Three-target fan-out: wire -> screen flash -> IR. Same order as
    // BeatPulse's per-beat shape so the failure modes are symmetric.
    DAL::render_fx("esp-now-broadcast", pulse);
    DAL::fire_display_clear("local", DisplayClearEvent{WHITE});
    DAL::render_fx("all-pixmobs", pulse);
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
