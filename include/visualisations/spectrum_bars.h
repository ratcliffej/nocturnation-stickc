// SpectrumBarsVisualisation - the first vis that consumes
// SpectrumFrameEvent (Epic 4.6 Block 11).
//
// Renders 32 vertical bars across the master LCD, heights driven by the
// 32-band log-spaced spectrum the analyser ships in SpectrumFrameEvent.
// Confirm is the manual sound-check trigger: fires a guaranteed pulse
// across the wire + IR + screen so the operator can validate IR reach
// without waiting for a beat.
//
// Power profile:
//   needs_audio_frames    = true   (so the vis can ride beats too)
//   needs_spectrum_frame  = true   (the load-bearing flag - Block 7's
//                                   gate flips the FFT path on when
//                                   this vis activates)
//   lcd_refresh_hz_max    = 30
//   tick_hz               = 0
//
// Required capabilities: Mic (audio frames are foundational; the
// spectrum path declares AnalyserSpectrumFrame implicitly via the
// host's HAL capabilities, which the picker capability-gate already
// checks against this vis when the host doesn't have the analyser).
//
// id() is "spec-bars" (9 chars). The brief warned that "spectrum-bars"
// would push the NVS namespace prefix "nv_<id>" past the 15-char limit.
//
// Properties:
//   "band_focus" Enum [0..3], {"All","Bass","Mid","Treble"}, default 0.
//      Tints the focused band(s) for visual emphasis. Bass = bands 0-9,
//      Mid = bands 10-21, Treble = bands 22-31 (rough log-spaced split
//      mirroring the analyser's 3-band roll-up boundaries).
//   "sensitivity" U8 [1..10], default 5.
//      Multiplies band magnitudes when computing bar height. Higher =
//      more visible at low volumes. No unit string.

#pragma once

#include <cstddef>
#include <cstdint>

#include "visualisations/visualisation.h"
#include "plugins/property_bag.h"

namespace nocturnation {
namespace visualisations {

class SpectrumBarsVisualisation : public Visualisation {
public:
    const char* id()           const override { return "spec-bars"; }
    const char* display_name() const override { return "Spectrum Bars"; }

    hal::CapabilityMask                       required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties()            const override;
    plugins::PowerProfile                     power()                 const override;

    void enter(VisualisationContext&) override;
    void exit (VisualisationContext&) override;

    void on_audio_frame   (VisualisationContext&,
                            const dal::AudioFrameEvent&)    override;
    void on_spectrum_frame(VisualisationContext&,
                            const dal::SpectrumFrameEvent&) override;
    void on_input_action  (VisualisationContext&,
                            const hal::InputEvent&)         override;

    VisualisationContext& context() override;

private:
    // Last spectrum render time, gated by lcd_refresh_hz_max (30 Hz).
    uint32_t last_draw_ms_ = 0;

    void draw_spectrum(VisualisationContext& ctx,
                       const dal::SpectrumFrameEvent& ev);
    void fire_manual_beat(VisualisationContext& ctx);

    // Returns true when band `i` is part of the currently focused band
    // group ("All" -> always true; Bass/Mid/Treble -> contiguous range).
    static bool band_is_focused(uint8_t band_focus, size_t i);

    // Returns the RGB565 tint for the focused band's fill colour. For
    // "All" the tint is white (no highlight); for Bass/Mid/Treble it's
    // a recognisable hue.
    static uint16_t focused_tint(uint8_t band_focus);

    // Returns 0x00RRGGBB packed bytes matching focused_tint, used for
    // the manual-beat PixMob pulse on Confirm.
    static void focused_rgb(uint8_t band_focus,
                            uint8_t& r, uint8_t& g, uint8_t& b);
};

// Singletons. main.cpp registers via:
//   visualisation_registry().register_plugin(spectrum_bars_instance());
SpectrumBarsVisualisation* spectrum_bars_instance();
plugins::PropertyBag&      spectrum_bars_property_bag();
VisualisationContext&      spectrum_bars_context();

}  // namespace visualisations
}  // namespace nocturnation
