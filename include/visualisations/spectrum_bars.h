// SpectrumBarsVisualisation - the first vis that consumes
// SpectrumFrameEvent (Epic 4.6 Block 11).
//
// Renders 32 vertical bars across the Director LCD, heights driven by the
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
// Visual: 7 perceptual bars (Sub Bass, Bass, Low Mids, Midrange, High
// Mids, Presence, Air) mapping the analyser's Audible-Genius perceptual
// boundaries. Each band has a permanent rainbow colour (purple -> red ->
// orange -> yellow -> green -> cyan -> light blue, warm-to-cool by
// frequency) and a 3-4 char label under the bar. The 32 log-spaced
// spectrum bands are rolled up into the 7 perceptual buckets per a
// hard-coded mapping table that matches the canonical 16 kHz operating
// point; the Mud band (<20 Hz) is folded into Sub Bass.
//
// Properties:
//   "sensitivity" U8 [1..10], default 5.
//      Multiplies the log-compressed band magnitude when computing
//      bar height. Higher = more visible at low volumes.

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

    // SpectrumBars paints 32 bars across the full Director LCD; the
    // mode's BeatPulse-era chrome (colour title, BPM line, flux meter)
    // would clobber the bars on every 50 ms loop_tick if it kept
    // running.
    bool wants_full_screen() const override { return true; }

private:
    // Last spectrum render time, gated by lcd_refresh_hz_max (30 Hz).
    uint32_t last_draw_ms_ = 0;

    void draw_spectrum(VisualisationContext& ctx,
                       const dal::SpectrumFrameEvent& ev);
    void fire_manual_beat(VisualisationContext& ctx);
};

// Singletons. main.cpp registers via:
//   visualisation_registry().register_plugin(spectrum_bars_instance());
SpectrumBarsVisualisation* spectrum_bars_instance();
plugins::PropertyBag&      spectrum_bars_property_bag();
VisualisationContext&      spectrum_bars_context();

}  // namespace visualisations
}  // namespace nocturnation
