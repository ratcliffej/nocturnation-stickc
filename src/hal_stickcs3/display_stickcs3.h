// M5StickS3 Display backend.
//
// Wraps M5.Display (LovyanGFX behind M5Unified). The S3 carries the same
// 1.14" ST7789 panel as the StickC Plus2 at the same 135x240 resolution,
// just on different SPI pins (SCK 40 / MOSI 39 / CS 41 / DC 45 / RST 21
// / BL 38). M5Unified's runtime board detection routes the SPI traffic
// to those pins automatically; the HAL surface is therefore identical
// to the Plus2 backend.

#pragma once

#include "hal/hal.h"
#include <M5Unified.h>

namespace nocturnation {
namespace hal {

class DisplayStickCS3 : public Display {
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
    void set_text_wrap(bool wrap_x, bool wrap_y) override;

    void flush() override;

    bool begin_buffered_paint(int x, int y, int w, int h) override;
    void end_buffered_paint() override;

private:
    LGFX_Sprite buffer_sprite_{ &M5.Display };
    bool        buffered_   = false;
    int         buffer_x_   = 0;
    int         buffer_y_   = 0;
    int         buffer_w_   = 0;
    int         buffer_h_   = 0;

    uint16_t    text_fg_    = 0xFFFF;
    uint16_t    text_bg_    = 0x0000;
    uint8_t     text_size_  = 1;
};

}  // namespace hal
}  // namespace nocturnation
