// Visualisation - abstract base for Director-side visualisation plugins.
//
// Concrete visualisations subclass this and override the hooks they
// care about. The framework owns one instance per registered vis and
// activates exactly one at a time (selected via the picker). Block 5
// lands the contract only; Block 8 migrates BeatPulse to it and lands
// the director_mode rewrite that drives the lifecycle, and
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

    // Singleton context accessor (Epic 4.6 Block 11). Each concrete vis
    // owns a single VisualisationContext singleton in its translation
    // unit; the framework (DirectorMode picker / settings path)
    // routes events through this accessor so it scales to any number of
    // registered visualisations without hardcoding per-id branches.
    virtual VisualisationContext& context() = 0;

    // Property-change notification (Epic 4.6 Block 11). The framework's
    // Settings overlay calls this after every successful set_property()
    // so a vis can re-sync any cached state derived from a property
    // value. BeatPulse uses this to refresh effects::Pulse's cached
    // colour - editing "color" via Settings would otherwise stay stale
    // inside pulse_ until the next Cycle action.
    //
    // Default: no-op. Vis that don't cache derived state ignore this.
    virtual void on_property_changed(VisualisationContext&, const char* /*key*/) {}

    // Screen-ownership flag (Epic 4.6 hotfix to Block 11). A vis that
    // returns true claims the Director LCD for its own rendering; the
    // mode skips its BeatPulse-era chrome (colour title, BPM line,
    // flux meter, footer) so the vis can paint freely. Vis that share
    // the screen with the mode chrome (BeatPulse) return false and
    // the mode redraws its UI on a 20 Hz cadence.
    //
    // Default: false (mode chrome stays). SpectrumBars overrides to
    // true. Picker / Settings overlays gate this upstream - while an
    // overlay is open the mode owns the screen regardless.
    virtual bool wants_full_screen() const { return false; }
};

}  // namespace visualisations
}  // namespace nocturnation
