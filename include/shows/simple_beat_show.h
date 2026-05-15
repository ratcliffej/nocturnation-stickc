// SimpleBeatShow - the Block 1 Show that preserves Epic 4.6's
// beat-pulse behaviour under the new Show plug-in framework.
//
// Wire output is byte-identical to BeatPulseVisualisation (the
// pre-Epic-4.7 Director-side renderer): every detected beat fires three
// render targets in the same order with the same envelopes:
//   1. DAL::render_fx("00:00", RgbPulseEvent{...})
//      -> broadcast to all classes / all groups (Epic 4.65 unified
//      target format; equivalent to the pre-4.65 "esp-now-broadcast"
//      device name).
//   2. DAL::fire_display_clear("local", DisplayClearEvent{colour})
//      -> screen flash on the Director itself (preserved as
//      DisplayClearEvent, not RgbPulseEvent).
//   3. effects::Pulse::on_beat(...) which fans out
//      render_fx("all-pixmobs", ...) -> IR via the PixMob driver on
//      the Director.
//
// Compared to BeatPulseVisualisation, SimpleBeatShow additionally
// owns the full screen rendering: status strip (show name), colour
// title, BPM readout, batt + IR fire counter, flux meter, operator
// hint footer. The on-screen flux meter consumes raw audio frames
// (on_audio_frame) so it remains live; the per-beat fan-out hangs
// off on_beat_detected which the mode fires when AudioFrameEvent.is_beat
// is true.
//
// Properties:
//   "color" : Enum, 0-5 (Off/Red/Green/Blue/Yellow/White), default Red.
//
// Power profile: needs_audio_frames=true (flux meter + beat tracking);
// no spectrum / 8-band needs. Capability requirements: Mic.

#pragma once

#include <cstddef>
#include <cstdint>

#include "shows/show.h"
#include "plugins/property_bag.h"
#include "widgets/beat_bar.h"

namespace nocturnation {
namespace shows {

class SimpleBeatShow : public Show {
public:
    const char* id()           const override { return "simple-beat"; }
    const char* display_name() const override { return "Simple Beat"; }

    hal::CapabilityMask                  required_capabilities() const override;
    plugins::Span<const plugins::PropertyDef> properties()        const override;
    plugins::PowerProfile                power()                  const override;

    void enter(ShowContext&) override;
    void exit (ShowContext&) override;

    void on_audio_frame    (ShowContext&, const dal::AudioFrameEvent&) override;
    void on_beat_detected  (ShowContext&, uint8_t strength) override;
    void on_input_action   (ShowContext&, const hal::InputEvent&) override;
    void on_render         (ShowContext&) override;

    // Per-show context accessor.
    ShowContext& context() override;

    // Test accessors (native builds): inspect internal state without
    // having to scrape the rendered framebuffer.
#ifndef ARDUINO
    float    estimated_bpm_for_tests()   const { return estimated_bpm_; }
    float    current_flux_for_tests()    const { return current_flux_; }
    float    baseline_flux_for_tests()   const { return baseline_flux_; }
#endif

private:
    static constexpr size_t kIbiBufferSize = 8;

    // Block 2: flux meter rendering moved into the BeatBarWidget
    // library. SimpleBeatShow's on_render owns the widget instance and
    // feeds it the current ratio / threshold every frame.
    widgets::BeatBarWidget flux_bar_;

    // Beat / BPM tracking (mirrors BeatPulseVisualisation exactly).
    uint32_t  last_beat_ms_   = 0;
    uint32_t  ibi_buffer_[kIbiBufferSize] = {0};
    size_t    ibi_index_      = 0;
    size_t    ibi_count_      = 0;
    float     estimated_bpm_  = 0.0f;

    // Flux meter state (display-only; mirrors pre-Block-1
    // DirectorMode's per-frame tracking that fed the meter).
    float     baseline_flux_    = 100.0f;
    float     prev_bass_energy_ = 0.0f;
    float     current_flux_     = 0.0f;
    float     current_level_    = 0.0f;

    void update_bpm_from_buffer();
};

// Singletons. main.cpp registers via:
//   show_registry().register_plugin(simple_beat_show_instance());
SimpleBeatShow*       simple_beat_show_instance();
plugins::PropertyBag& simple_beat_show_property_bag();
ShowContext&          simple_beat_show_context();

}  // namespace shows
}  // namespace nocturnation
