// M5StickC Plus2 Display backend.
//
// Wraps M5.Display (LovyanGFX behind M5Unified). M5.begin() in main.cpp
// performs the actual hardware init; this backend's begin() is a no-op,
// which is the right behaviour while main.cpp still owns top-level setup.
//
// All draw operations delegate directly to the LovyanGFX object - this
// is the only thing in the firmware that's allowed to reference M5.Display.
//
// Buffered paint sessions (begin_buffered_paint / end_buffered_paint) use
// an LGFX_Sprite as an off-screen scratch area; draws made between begin
// and end accumulate into the sprite and end pushes the whole thing in
// a single SPI burst. Reduces tearing on multi-element redraws (status
// strip with multiple icons + text). For single solid-colour fills the
// sprite buys nothing - same SPI work either way - but the API stays
// uniform so callers don't need a fast-path.

#pragma once

#include "hal/hal.h"
#include <M5Unified.h>

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

    bool begin_buffered_paint(int x, int y, int w, int h) override;
    void end_buffered_paint() override;

private:
    // Off-screen scratch sprite, used between begin/end_buffered_paint.
    // The sprite covers (buffer_x_, buffer_y_, buffer_w_, buffer_h_) in
    // screen space; draws are translated to local sprite coordinates.
    LGFX_Sprite buffer_sprite_{ &M5.Display };
    bool        buffered_   = false;
    int         buffer_x_   = 0;
    int         buffer_y_   = 0;
    int         buffer_w_   = 0;
    int         buffer_h_   = 0;

    // Text state propagated to the sprite when buffering is active so
    // draw_text uses the operator-set fg/bg/size while in a session.
    uint16_t    text_fg_    = 0xFFFF;
    uint16_t    text_bg_    = 0x0000;
    uint8_t     text_size_  = 1;
};

}  // namespace hal
}  // namespace nocturnation
