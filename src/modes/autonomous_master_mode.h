// AutonomousMasterMode - lifts the Epic 1 beat-detection + IR-sending logic
// out of main.cpp. Behaviour is preserved exactly; the only change is that
// PWR long-press now goes to the Menu instead of toggling beat-mode.

#pragma once

#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"          // public header in include/
#include "effects/effects.h"

namespace nocturnation {
namespace modes {

namespace autonomous_master_detail {

// Used by AutonomousMasterMode (preserves Epic 1's six-state cycle including
// the OFF "skip IR" state). Lives here only because AutonomousMasterMode
// holds one as a member; helpers live in the .cpp anon namespace.
enum class Colour : uint8_t {
    Off = 0, Red, Green, Blue, Yellow, White
};

}  // namespace autonomous_master_detail

class AutonomousMasterMode : public Mode {
public:
    ModeId id() const override { return ModeId::AutonomousMaster; }
    const char* name() const override { return "Autonomous Master"; }

    AutonomousMasterMode();

    void enter() override;
    void exit() override;
    void loop_tick() override;
    void on_audio_frame(const dal::AudioFrameEvent& ev) override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    static constexpr size_t kIbiBufferSize = 8;

    // Channel comes from NVS (Config > ESP-NOW > Master Channel) per
    // spec §4.5: 1 = hobby (default), 11 = show, 6 = advanced override.
    // The radio itself lives in EspNowBroadcastDriver - this mode just
    // starts/stops broadcast in its enter/exit lifecycle and hits the
    // wire via DAL::render_fx("esp-now-broadcast", ...) and the driver's
    // send_music_event() entry point.

    autonomous_master_detail::Colour colour_ =
        autonomous_master_detail::Colour::Red;
    bool              paused_           = false;
    effects::Pulse    pulse_;

    float     baseline_flux_    = 100.0f;
    float     prev_bass_energy_ = 0.0f;
    float     current_flux_     = 0.0f;
    float     current_level_    = 0.0f;
    uint32_t  last_beat_ms_     = 0;
    uint32_t  last_draw_ms_     = 0;

    uint32_t  ibi_buffer_[kIbiBufferSize] = {0};
    size_t    ibi_index_        = 0;
    size_t    ibi_count_        = 0;
    float     estimated_bpm_    = 0.0f;

    static void delay_ms(uint32_t ms);

    void sync_pulse_colour();
    void update_bpm_from_buffer();
    void draw();
};

}  // namespace modes
}  // namespace nocturnation
