// BeatPulseVisualisation - the existing single-colour beat-driven pulse,
// migrated from DirectorMode in Epic 4.6 Block 8.
//
// Properties:
//   "color" : Enum, 0-5 (Off/Red/Green/Blue/Yellow/White), default Red.
//
// Power profile: needs_audio_frames=true; doesn't need spectrum or
// 8-band summary (pure beat-driven). Capability requirements: Mic
// (the vis is a no-op without audio frames).
//
// Wire output is byte-identical to the pre-migration implementation:
// every detected beat fires three explicit render_fx targets in the
// same order the existing code did:
//   1. DAL::render_fx("esp-now-broadcast", RgbPulseEvent{...})
//      -> wire to slaves
//   2. DAL::fire_display_clear("local", DisplayClearEvent{...})
//      -> screen flash (preserved as-is, NOT RgbPulseEvent)
//   3. effects::Pulse::on_beat(...) which internally fires
//      render_fx("all-pixmobs", ...) -> IR via PixMob driver

#pragma once

#include <cstddef>
#include <cstdint>

#include "visualisations/visualisation.h"
#include "plugins/property_bag.h"
#include "effects/effects.h"

namespace nocturnation {
namespace visualisations {

class BeatPulseVisualisation : public Visualisation {
public:
    const char* id()           const override { return "beat-pulse"; }
    const char* display_name() const override { return "Beat Pulse"; }

    hal::CapabilityMask                  required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties()        const override;
    plugins::PowerProfile                power()                  const override;

    void enter(VisualisationContext&) override;
    void exit (VisualisationContext&) override;
    void on_audio_frame (VisualisationContext&, const dal::AudioFrameEvent&) override;
    void on_input_action(VisualisationContext&, const hal::InputEvent&)      override;
    void on_property_changed(VisualisationContext&, const char* key)         override;

    // Per-vis context accessor (Block 11). Returns the BeatPulse-owned
    // VisualisationContext singleton defined in beat_pulse.cpp.
    VisualisationContext& context() override;

    // Status-display accessor: current BPM tracked by the vis. AutonomousMaster
    // draw() reads this through beat_pulse_estimated_bpm() so the mode's status
    // display stays consistent with what the vis is acting on.
    float estimated_bpm() const { return estimated_bpm_; }

private:
    static constexpr size_t kIbiBufferSize = 8;

    effects::Pulse pulse_{"all-pixmobs"};
    uint32_t  last_beat_ms_   = 0;
    uint32_t  ibi_buffer_[kIbiBufferSize] = {0};
    size_t    ibi_index_      = 0;
    size_t    ibi_count_      = 0;
    float     estimated_bpm_  = 0.0f;

    void update_bpm_from_buffer();
    void sync_pulse_colour(VisualisationContext&);
};

// Singletons. main.cpp registers via:
//   visualisation_registry().register_plugin(beat_pulse_instance());
BeatPulseVisualisation* beat_pulse_instance();
plugins::PropertyBag&   beat_pulse_property_bag();
VisualisationContext&   beat_pulse_context();

// Status-display accessors used by DirectorMode::draw(). They read
// the current "color" enum out of the singleton property bag and map it via
// the same lookup helpers the pre-migration mode used.
const char* beat_pulse_colour_label();
uint16_t    beat_pulse_colour_screen_rgb();
float       beat_pulse_estimated_bpm();

}  // namespace visualisations
}  // namespace nocturnation
