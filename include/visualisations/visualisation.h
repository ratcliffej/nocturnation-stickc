// Visualisation - abstract base for master-side visualisation plugins.
//
// Concrete visualisations subclass this and override the hooks they
// care about. The framework owns one instance per registered vis and
// activates exactly one at a time (selected via the picker). Block 5
// lands the contract only; Block 8 migrates BeatPulse to it and lands
// the autonomous_master_mode rewrite that drives the lifecycle, and
// Block 11 adds Spectrum Bars on top.
//
// Lifetime:
//   - enter(ctx) / exit(ctx) bracket the active period.
//   - on_audio_frame / on_spectrum_frame fire from the analyser's mic
//     frame callback when the vis is active AND its power profile
//     declares need for that surface (see PowerProfile in plugin.h).
//   - on_input_action fires when the user produces an InputEvent
//     while this vis is active. Picker / Settings / Pause overlays
//     intercept their own actions upstream; what reaches the vis is
//     the vis-specific subset.
//   - tick(ctx, now_ms) fires from the framework's loop_tick at the
//     cadence declared by power().tick_hz (0 = never; vis is purely
//     audio-driven).

#pragma once

#include <cstdint>

#include "plugins/plugin.h"
#include "dal/dal.h"
#include "hal/input_action.h"

namespace nocturnation {
namespace visualisations {

class VisualisationContext;   // forward declaration

class Visualisation : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override { return plugins::PluginKind::Visualisation; }

    virtual void enter(VisualisationContext&) {}
    virtual void exit (VisualisationContext&) {}

    virtual void on_audio_frame   (VisualisationContext&, const dal::AudioFrameEvent&)    {}
    virtual void on_spectrum_frame(VisualisationContext&, const dal::SpectrumFrameEvent&) {}
    virtual void on_input_action  (VisualisationContext&, const hal::InputEvent&)         {}
    virtual void tick             (VisualisationContext&, uint32_t now_ms)                {}
};

}  // namespace visualisations
}  // namespace nocturnation
