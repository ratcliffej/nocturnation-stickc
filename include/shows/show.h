// Show - abstract base for Director-side performance plug-ins (Epic 4.7).
//
// A Show is the unit of "what is the Director doing right now": it
// consumes analyser events, makes render_fx() calls with class+group
// targets, owns the screen, and owns button handling beyond the
// back-gesture. DirectorMode holds exactly one active Show at
// a time, selectable via the Director-mode picker or ConfigMode > Show.
//
// Compared to the Epic 4.6 Visualisation plug-in: a Show is a richer
// surface that *owns* the screen (rather than rendering inside the
// mode's chrome), receives both raw analyser frames and high-level
// analyser events (on_beat_detected and the Block 3/4 additions), and
// is the seam new performances drop into. The existing Visualisation
// classes (BeatPulse, SpectrumBars) become a widget library in
// Block 2; their per-beat fan-out logic moves into Show subclasses
// like SimpleBeatShow (Block 1) and DynamicShow (Block 5).
//
// Block 1 ships the contract + SimpleBeatShow. Block 3 wires up the
// analyser primitive hooks (on_snare_detected, on_hihat_detected,
// on_music_descriptor); Block 4 wires up on_section_change. The base
// declares all hooks now with no-op defaults so the API surface is
// stable from Block 1 onwards.
//
// Lifetime:
//   - enter(ctx) / exit(ctx) bracket the active period.
//   - on_audio_frame / on_spectrum_frame fire from the analyser's mic
//     frame callback when the show is active AND its power profile
//     declares need for that surface (see PowerProfile in plugin.h).
//   - on_beat_detected fires when AudioFrameEvent.is_beat is true; the
//     mode dispatches alongside on_audio_frame so a show can subscribe
//     at either level. on_snare_detected / on_hihat_detected fire in
//     Block 3 once the multi-band analyser primitives land.
//   - on_input_action fires when the user produces an InputEvent
//     while this show is active. Picker / Settings overlays intercept
//     their own actions upstream; what reaches the show is the show-
//     specific subset (Cycle / Confirm / CyclePrev).
//   - on_render fires at the mode's draw cadence (50 ms) when no
//     overlay is open; the show owns the canvas - it should clear and
//     paint its full screen each call.
//   - tick(ctx, now_ms) fires from the framework's loop_tick at the
//     cadence declared by power().tick_hz (0 = never; show is purely
//     audio/event-driven).

#pragma once

#include <cstdint>

#include "plugins/plugin.h"
#include "dal/dal.h"
#include "hal/input_action.h"

namespace nocturnation {
namespace shows {

class ShowContext;   // forward declaration

class Show : public plugins::Plugin {
public:
    plugins::PluginKind kind() const override { return plugins::PluginKind::Show; }

    // Lifecycle
    virtual void enter(ShowContext&) {}
    virtual void exit (ShowContext&) {}

    // Raw analyser frames (optional - shows that only want high-level
    // events can ignore these and override the on_*_detected hooks).
    virtual void on_audio_frame   (ShowContext&, const dal::AudioFrameEvent&)    {}
    virtual void on_spectrum_frame(ShowContext&, const dal::SpectrumFrameEvent&) {}

    // Analyser events. Block 1 fires on_beat_detected (when is_beat is
    // true on an AudioFrameEvent). Blocks 3-4 add the firing side for
    // snare/hihat/descriptor/section once the analyser primitives land.
    // strength: 0..255, normalised by the analyser (Block 1 passes 255
    // as a placeholder; Block 3 surfaces the actual per-band magnitude).
    virtual void on_beat_detected   (ShowContext&, uint8_t /*strength*/) {}
    virtual void on_snare_detected  (ShowContext&, uint8_t /*strength*/) {}
    virtual void on_hihat_detected  (ShowContext&, uint8_t /*strength*/) {}

    // Continuous music descriptors fired at FFT rate, rate-limited so
    // only meaningful changes (> 5 % from previous) reach this hook.
    // centroid: tonal centre (0=bass, 255=bright)
    // energy:   overall loudness envelope
    // density:  events-per-second across all bands
    virtual void on_music_descriptor(ShowContext&,
                                       uint8_t /*centroid*/,
                                       uint8_t /*energy*/,
                                       uint8_t /*density*/) {}

    // Section transition (Block 4). section: 0=UNKNOWN, 1=VERSE,
    // 2=CHORUS, 3=BUILDUP, 4=BREAKDOWN, 5=VOCALS_ONLY,
    // 6=INSTRUMENTAL_BREAK, 7=DROP.
    virtual void on_section_change(ShowContext&, uint8_t /*section*/) {}

    // Input. Cycle / Confirm / CyclePrev reach the show. Picker /
    // Settings actions are consumed by the mode upstream.
    virtual void on_input_action(ShowContext&, const hal::InputEvent&) {}

    // Per-frame screen draw. Fired at the mode's draw cadence (50 ms)
    // when no overlay is open. The show owns the canvas: it should
    // clear and paint its full screen each call.
    virtual void on_render(ShowContext&) {}

    // Property-change notification. Mirrors Visualisation's hook so
    // shows can re-sync any cached state derived from a property
    // value after the Settings overlay updates the bag.
    virtual void on_property_changed(ShowContext&, const char* /*key*/) {}

    // Tick at power().tick_hz cadence (0 = never; show is event-driven).
    virtual void tick(ShowContext&, uint32_t /*now_ms*/) {}

    // Per-Show singleton context accessor. Each concrete show owns a
    // ShowContext singleton in its translation unit and exposes it
    // here so the mode can route events without hard-coding per-id
    // branches. Mirrors Visualisation::context().
    virtual ShowContext& context() = 0;
};

}  // namespace shows
}  // namespace nocturnation
