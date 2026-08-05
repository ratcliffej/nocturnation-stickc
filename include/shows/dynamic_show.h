// DynamicShow - the Block 5 headline Show that consumes the
// Block 3-4 analyser primitives and routes effects to different
// device groups via the Epic 4.65 class+group target format.
//
// Routing (PixMob-oriented; targets follow the operator-workflow.md
// deployment convention that PixMobs live in groups 10..12):
//   kick  (on_beat_detected)  -> render_fx("01:0a", ev)  Light, group 10
//   snare (on_snare_detected) -> render_fx("01:0b", ev)  Light, group 11
//   hi-hat (on_hihat_detected)-> render_fx("01:0c", ev)  Light, group 12
//
// Colour:
//   centroid -> hue   (low = cool blue, high = warm red)
//   energy   -> brightness / value
//   section  -> hue offset + brightness modifier (VERSE cooler, BUILDUP
//              warmer, BREAKDOWN dim, DROP overrides to white)
//
// Density -> bracelet response chance per fire so sparse music
// produces sparse coverage while busy music lights everyone.
//
// Screen: status + current section label + descriptor readouts +
// SpectrumBarsWidget driven from the AudioFrameEvent's per-band
// perceptual summary so the operator can see live levels alongside
// the section state.
//
// Properties:
//   "groups" : U8, 1..3, default 1. How the kick / snare / hi-hat
//              streams distribute across PixMob bracelet groups.
//                1 -> all events broadcast to group 0 (every bracelet
//                     responds regardless of its programmed group).
//                     Default - works out of the box on any deployment.
//                2 -> kick to group 10, snare + hi-hat both to group 11.
//                     Pair-of-groups split for two-tier deployments.
//                3 -> kick to group 10, snare to group 11, hi-hat to
//                     group 12. Requires bracelets pre-programmed into
//                     these three groups; full per-drum separation.
//
// The property is a mode selector (1, 2, or 3 groups active), NOT a
// target group value. Target group values are fixed at 10, 11, 12 to
// keep PixMob traffic out of the 1..9 range reserved for capability-
// rich devices (Tildagons, LED strips). See operator-workflow.md.
//
// Power profile: needs_audio_frames=true (drives the spectrum widget +
// BPM tracking via IBI buffer). Capability requirements: Mic.

#pragma once

#include <cstddef>
#include <cstdint>

#include "shows/show.h"
#include "plugins/property_bag.h"
#include "widgets/spectrum_bars.h"

namespace nocturnation {
namespace shows {

class DynamicShow : public Show {
public:
    const char* id()           const override { return "dynamic"; }
    const char* display_name() const override { return "Dynamic"; }

    hal::CapabilityMask                  required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties()         const override;
    plugins::PowerProfile                power()                 const override;

    void enter(ShowContext&) override;
    void exit (ShowContext&) override;

    void on_audio_frame      (ShowContext&, const dal::AudioFrameEvent&) override;
    void on_beat_detected    (ShowContext&, uint8_t strength) override;
    void on_snare_detected   (ShowContext&, uint8_t strength) override;
    void on_hihat_detected   (ShowContext&, uint8_t strength) override;
    void on_music_descriptor (ShowContext&,
                                uint8_t centroid,
                                uint8_t energy,
                                uint8_t density) override;
    void on_section_change   (ShowContext&, uint8_t section) override;
    void on_render           (ShowContext&) override;

    ShowContext& context() override;

#ifndef ARDUINO
    // Test accessors (native builds): inspect descriptor / section state.
    uint8_t  centroid_for_tests() const { return centroid_; }
    uint8_t  energy_for_tests()   const { return energy_; }
    uint8_t  density_for_tests()  const { return density_; }
    uint8_t  section_for_tests()  const { return section_; }
    float    estimated_bpm_for_tests() const { return estimated_bpm_; }
#endif

private:
    static constexpr size_t kIbiBufferSize = 8;

    // Latest descriptor + section state cached from the hooks so the
    // fire path can resolve a colour without re-reading the audio
    // frame each event. Defaults are mid-range so the first beat fires
    // a sensible colour before any descriptor has arrived.
    uint8_t  centroid_ = 128;
    uint8_t  energy_   = 128;
    uint8_t  density_  = 0;
    uint8_t  section_  = 0;     // SectionType::Unknown

    // Per-frame perceptual bands cached for the on-screen spectrum
    // widget. Matches widgets::kSpectrumBandCount (7).
    float    band_values_[7]   = {0, 0, 0, 0, 0, 0, 0};

    // BPM tracking (kick events) for the envelope picker.
    uint32_t last_beat_ms_                 = 0;
    uint32_t ibi_buffer_[kIbiBufferSize]   = {0};
    size_t   ibi_index_                    = 0;
    size_t   ibi_count_                    = 0;
    float    estimated_bpm_                = 0.0f;

    // Single SpectrumBarsWidget for the on-screen display.
    widgets::SpectrumBarsWidget spectrum_widget_;

    void fire_event(ShowContext&, const char* target, uint8_t strength);
    void update_bpm_from_buffer();
};

// Singletons. main.cpp registers via:
//   show_registry().register_plugin(dynamic_show_instance());
DynamicShow*          dynamic_show_instance();
plugins::PropertyBag& dynamic_show_property_bag();
ShowContext&          dynamic_show_context();

}  // namespace shows
}  // namespace nocturnation
