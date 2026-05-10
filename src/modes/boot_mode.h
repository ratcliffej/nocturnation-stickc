// BootMode - 5s countdown screen, button-press interrupt.

#pragma once

#include <cstdint>

#include "modes/mode_machine.h"

namespace nocturnation {
namespace modes {

class BootMode : public Mode {
public:
    ModeId id() const override { return ModeId::Boot; }
    const char* name() const override { return "Boot"; }

    void enter() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    uint32_t start_ms_           = 0;
    uint8_t  last_drawn_seconds_ = 0xFF;
    uint8_t  last_drawn_pulse_   = 0xFF;

    // Layout. Title is "Noctur" + pulsing "N" + "ation" at size 3 so the
    // brand mark matches the website banner. Size-3 char cells are 18 px
    // wide; 12 chars * 18 = 216 px, leaves ~12 px margin on each side of
    // the 240 px display.
    static constexpr int kTitleX  = 12;
    static constexpr int kTitleY  = 20;
    static constexpr int kCharW3  = 18;
    static constexpr int kPulseNX = kTitleX + 6 * kCharW3;   // start of 2nd N

    static uint16_t pulse_color(uint8_t step);
    static const char* mode_label(ModeId m);

    void draw_static();
    void draw_countdown(uint8_t seconds);
    void draw_pulsing_n(uint8_t step);
};

}  // namespace modes
}  // namespace nocturnation
