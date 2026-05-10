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
//
// Block 10 lands:
//   - active-vis resolution from NVS via persistence::load_active_vis_id
//   - InputAction-driven control (via Mode::on_input_action)
//   - Picker overlay: enumerate the registry, pick a vis or return to menu
//   - Settings overlay: auto-generated UI driven by the active vis's
//     PropertyDef schema
//   - Render gating while an overlay is open (vis BPM tracking still
//     updates; render fan-out from master's screen is skipped so the
//     overlay isn't overpainted)
//   - Status-strip label showing the active vis name on the main screen.

#pragma once

#include <cstdint>

#include "modes/mode_machine.h"          // public header in include/
#include "hal/input_action.h"

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
    void on_audio_frame (const dal::AudioFrameEvent& ev) override;
    void on_input_action(const hal::InputEvent&      ev) override;

    // Test accessors. Native test envs reach into these to verify the
    // overlay state machine and the status-strip label content without
    // having to scrape a rendered framebuffer.
#ifndef ARDUINO
    enum class OverlayKind { None, Picker, Settings };
    OverlayKind overlay_for_tests() const {
        return static_cast<OverlayKind>(static_cast<int>(overlay_));
    }
    size_t      overlay_cursor_for_tests() const { return overlay_cursor_; }
    const char* active_vis_id_for_tests() const;
    const char* status_label_for_tests()  const { return status_label_buf_; }
#endif

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

    // Overlay state. Picker enumerates the registry; Settings walks
    // active_vis_'s PropertyDef schema. Both consume InputActions and
    // gate the vis render fan-out (so the screen-pulse fade doesn't
    // overpaint the overlay).
    enum class Overlay : uint8_t { None = 0, Picker = 1, Settings = 2 };
    Overlay   overlay_           = Overlay::None;
    size_t    overlay_cursor_    = 0;

    // Truncated active-vis name; refreshed on enter() and on picker
    // confirmation. ~10 chars + ellipsis -> 16 bytes is generous.
    static constexpr size_t kStatusLabelCap = 16;
    char      status_label_buf_[kStatusLabelCap] = {0};

    void resolve_active_vis_from_nvs();
    void refresh_status_label();

    void draw();
    void draw_picker();
    void draw_settings();

    // Picker uses a contiguous index space: [0..count-1] = registered vis,
    // [count] = "<- Menu" sentinel. Helpers below resolve cursor positions.
    size_t picker_row_count() const;
    bool   picker_row_is_back(size_t row) const;

    // Settings uses an analogous space: [0..props.size-1] = properties,
    // [props.size] = "<- Back" sentinel. With no properties the back row
    // sits at index 0 alongside a "No settings" body.
    size_t settings_row_count() const;
    bool   settings_row_is_back(size_t row) const;

    void on_picker_confirm();
    void on_settings_confirm();
};

}  // namespace modes
}  // namespace nocturnation
