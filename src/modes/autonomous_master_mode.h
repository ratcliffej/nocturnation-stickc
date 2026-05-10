// AutonomousMasterMode - thin shell that hosts the active master-side
// Visualisation (Epic 4.6 Block 8 onwards).
//
// Pre-Block-8 this owned the beat-pulse logic, the IBI/BPM tracking, and
// the Colour enum directly. Post-Block-8 the per-beat render fan-out and
// the BPM tracking live in BeatPulseVisualisation; this mode shrinks to:
//   - lifecycle wiring (audio input, ESP-NOW broadcast)
//   - pause state (canonical here; mirrored into ctx)
//   - flux/baseline tracking for the on-screen meter (display-only)
//   - music_event broadcast (mode concern, not vis)
//   - status display (composes labels from the vis property bag)
//   - button-event routing (cycles colour via InputAction::Cycle on the vis)
//
// Block 10 will introduce the vis picker that lets the user choose between
// multiple registered visualisations and persist the choice to NVS. For
// Block 8 the active vis is hardcoded to BeatPulse via the registry.

#pragma once

#include <cstdint>

#include "modes/mode_machine.h"          // public header in include/

namespace nocturnation {

namespace visualisations { class Visualisation; class VisualisationContext; }

namespace modes {

class AutonomousMasterMode : public Mode {
public:
    ModeId id() const override { return ModeId::AutonomousMaster; }
    const char* name() const override { return "Autonomous Master"; }

    AutonomousMasterMode() = default;

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_audio_frame(const dal::AudioFrameEvent& ev) override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    // Channel comes from NVS (Config > ESP-NOW > Master Channel) per
    // spec §4.5. The radio itself lives in EspNowBroadcastDriver - this
    // mode just starts/stops broadcast in enter/exit; the driver's
    // loop_tick handles retransmits and the 1 Hz heartbeat.

    visualisations::Visualisation*        active_vis_ = nullptr;
    visualisations::VisualisationContext* ctx_        = nullptr;

    bool      paused_           = false;

    // Flux/baseline state stays at mode level - it powers the on-screen
    // meter, which is mode UI, not vis output. Beat firing is driven by
    // ev.is_beat (BeatDetector in DAL); these are diagnostic.
    float     baseline_flux_    = 100.0f;
    float     prev_bass_energy_ = 0.0f;
    float     current_flux_     = 0.0f;
    float     current_level_    = 0.0f;
    uint32_t  last_draw_ms_     = 0;

    void draw();
};

}  // namespace modes
}  // namespace nocturnation
