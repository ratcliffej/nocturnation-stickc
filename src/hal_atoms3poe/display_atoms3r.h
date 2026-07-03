// M5 AtomS3R Display backend (Director PoE variant).
//
// Wraps M5.Display (LovyanGFX behind M5Unified) for the AtomS3R's 0.85"
// 128x128 GC9107 panel. M5Unified's runtime board detection brings the
// panel up in M5.begin(); this class is the same thin forwarding shim as
// DisplayStickCS3, including the buffered-paint sprite session.
//
// Compiled only under NOCT_ATOMS3R (the m5stack-atoms3r-poe env). The
// AtomS3 Lite Director / repeater builds compile the .cpp to nothing and
// never reference this header.

#pragma once

#include "hal/hal.h"
#include <M5Unified.h>

namespace nocturnation {
namespace hal {

class DisplayAtomS3R : public Display {
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
