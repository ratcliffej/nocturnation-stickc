// M5StickC Plus2 Display backend.
//
// Wraps M5.Display (LovyanGFX behind M5Unified). M5.begin() in main.cpp
// performs the actual hardware init; this backend's begin() is a no-op,
// which is the right behaviour while main.cpp still owns top-level setup.
//
// All draw operations delegate directly to the LovyanGFX object - this
// is the only thing in the firmware that's allowed to reference M5.Display.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class DisplayStickCplus2 : public Display {
public:
    void begin() override;
    void set_rotation(uint8_t rotation) override;
    int  width() const override;
    int  height() const override;

    void clear(uint16_t color) override;
    void fill_rect(int x, int y, int w, int h, uint16_t color) override;
    void draw_rect(int x, int y, int w, int h, uint16_t color) override;
    void draw_hline(int x, int y, int w, uint16_t color) override;
    void draw_vline(int x, int y, int h, uint16_t color) override;

    void set_text_color(uint16_t fg, uint16_t bg) override;
    void set_text_size(uint8_t size) override;
    void draw_text(int x, int y, const char* text) override;

    void flush() override;
};

}  // namespace hal
}  // namespace nocturnation
