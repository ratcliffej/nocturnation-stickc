// DirectorMode - thin shell that hosts the active Director-side
// Show (Epic 4.7 Block 1 onwards).
//
// Pre-Epic-4.7 this hosted a Visualisation; Block 1 swaps that for the
// Show plug-in framework. Shows own the screen, button handling
// (beyond the back-gesture), and decide what gets sent on the wire.
// The mode shrinks to:
//   - lifecycle wiring (audio input, ESP-NOW broadcast)
//   - pause state (canonical here; mirrored into ShowContext)
//   - music_event broadcast (mode-level transport concern, not show
//     concern; pre-empts the broadcast even when the show would
//     otherwise be paused for the wider deployment)
//   - picker / settings overlays (mode-owned UI that takes over the
//     screen while open)
//   - dispatching analyser events (raw frames + on_beat_detected) to
//     the active show
//
// Compared to the pre-Block-1 mode: flux meter / colour title / BPM
// readout / batt+IR counter no longer live here - SimpleBeatShow paints
// them inside its on_render(). Mode chrome is gone except during the
// picker / settings overlays.

#pragma once

#include <cstdint>

#include "modes/mode_machine.h"          // public header in include/
#include "hal/input_action.h"

namespace nocturnation {

namespace shows { class Show; class ShowContext; }

namespace modes {

class DirectorMode : public Mode {
public:
    ModeId id() const override { return ModeId::Director; }
    const char* name() const override { return "Director"; }

    DirectorMode() = default;

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_audio_frame   (const dal::AudioFrameEvent&    ev) override;
    void on_spectrum_frame(const dal::SpectrumFrameEvent& ev) override;
    void on_input_action  (const hal::InputEvent&         ev) override;

    // Test accessors. Native test envs reach into these to verify the
    // overlay state machine and the active-show resolver without having
    // to scrape a rendered framebuffer.
#ifndef ARDUINO
    enum class OverlayKind { None, Picker, Settings };
    OverlayKind overlay_for_tests() const {
        return static_cast<OverlayKind>(static_cast<int>(overlay_));
    }
    size_t      overlay_cursor_for_tests() const { return overlay_cursor_; }
    const char* active_show_id_for_tests() const;
    // Convenience: returns the active show's display_name() or "" if
    // none. Kept as a separate accessor so the test surface stays
    // stable across "active show changes" rewrites.
    const char* status_label_for_tests()  const;
#endif

private:
    // Channel comes from NVS (Config > Connectivity > ESP-NOW > Director
    // Channel) per spec §4.5. The radio itself lives in
    // EspNowBroadcastDriver - this mode just starts / stops broadcast
    // in enter / exit; the driver's loop_tick handles retransmits and
    // the 1 Hz heartbeat.

    shows::Show*        active_show_ = nullptr;
    shows::ShowContext* ctx_         = nullptr;

    bool      paused_           = false;
    uint32_t  last_draw_ms_     = 0;

    // Last delivered MUSIC_DESCRIPTOR values (Epic 4.7 Block 3). The
    // descriptors are computed every audio frame (~30-40 Hz) but
    // delivery to Show::on_music_descriptor is rate-limited: a value
    // only reaches the show when any component has moved by more than
    // kMusicDescriptorDelta from the previously-delivered values. The
    // _delivered_ flag marks the first delivery so the very first
    // descriptor always reaches the show.
    bool      descriptor_delivered_       = false;
    uint8_t   last_centroid_delivered_    = 0;
    uint8_t   last_energy_delivered_      = 0;
    uint8_t   last_density_delivered_     = 0;
    static constexpr uint8_t kMusicDescriptorDelta = 13;   // ~5 % of 255

    // Last delivered SECTION (Epic 4.7 Block 4). Show::on_section_change
    // fires only when the inbound section value differs from the last
    // delivered. _delivered_ flags first-fire so the initial section
    // (typically UNKNOWN) reaches the show.
    bool      section_delivered_          = false;
    uint8_t   last_section_delivered_     = 0;

    // Overlay state. Picker enumerates show_registry; Settings walks
    // active_show_'s PropertyDef schema. Both consume InputActions and
    // own the screen while open so the Show's on_render() is skipped.
    enum class Overlay : uint8_t { None = 0, Picker = 1, Settings = 2 };
    Overlay   overlay_           = Overlay::None;
    size_t    overlay_cursor_    = 0;
    // Top-of-window for the overlay's visible scroll region. draw_picker
    // and draw_settings render at most kOverlayMaxVisible rows starting
    // from this index; update_overlay_view_offset() keeps the cursor
    // inside the window after every cursor advance.
    size_t    overlay_view_offset_ = 0;
    static constexpr size_t kOverlayMaxVisible = 5;
    void update_overlay_view_offset(size_t row_count);

    void resolve_active_show_from_nvs();

    // (Re-)subscribe / unsubscribe to spectrum frames based on the
    // active show's PowerProfile.needs_spectrum_frame. Drives Block 7's
    // pipeline gate live - shows that don't need spectrum keep the FFT
    // per-frame fan-out skipped in LocalDriver. Idempotent.
    void sync_spectrum_subscription(bool active);

    void draw();
    void draw_picker();
    void draw_settings();

    // Picker uses a contiguous index space:
    //   [0..count-1]   = registered shows
    //   [count]        = "DMX Bridge" entry (Epic 7 Q4 - external input
    //                    source replaces local interaction)
    //   [count+1]      = "<- Menu" sentinel
    size_t picker_row_count() const;
    bool   picker_row_is_back(size_t row) const;
    bool   picker_row_is_dmx_bridge(size_t row) const;

    // Settings uses an analogous space: [0..props.size-1] = properties,
    // [props.size] = "<- Back" sentinel.
    size_t settings_row_count() const;
    bool   settings_row_is_back(size_t row) const;

    void on_picker_confirm();
    void on_settings_confirm();
};

}  // namespace modes
}  // namespace nocturnation
