// M5 AtomS3R Display backend - compiled only for the AtomS3R Director
// (NOCT_ATOMS3R); collapses to nothing on the AtomS3 Lite Director and
// repeater builds that share this HAL folder.

#if defined(NOCT_ATOMS3R)

#include "display_atoms3r.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void DisplayAtomS3R::begin() {
    // Panel itself is initialised by HAL::begin()'s M5.begin() call
    // earlier. Rotation 0 = portrait with (0,0) at the top-left when the
    // Atom sits on the PoE base, USB/Grove edge down - the natural
    // orientation for a unit racked next to the switch.
    M5.Display.setRotation(0);
    // The dashboard is glanced at across a dim stage rack; M5Unified's
    // default backlight is conservative. ~2/3 keeps it readable without
    // meaningfully denting the PoE power budget.
    M5.Display.setBrightness(160);
}

void DisplayAtomS3R::set_rotation(uint8_t rotation) { M5.Display.setRotation(rotation); }
int  DisplayAtomS3R::width() const                  { return M5.Display.width(); }
int  DisplayAtomS3R::height() const                 { return M5.Display.height(); }

void DisplayAtomS3R::clear(uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(0, 0, buffer_w_, buffer_h_, color);
    } else {
        M5.Display.fillScreen(color);
    }
}

void DisplayAtomS3R::fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.fillRect(x, y, w, h, color);
    }
}

void DisplayAtomS3R::draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.drawRect(x, y, w, h, color);
    }
}

void DisplayAtomS3R::draw_hline(int x, int y, int w, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastHLine(x - buffer_x_, y - buffer_y_, w, color);
    } else {
        M5.Display.drawFastHLine(x, y, w, color);
    }
}

void DisplayAtomS3R::draw_vline(int x, int y, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastVLine(x - buffer_x_, y - buffer_y_, h, color);
    } else {
        M5.Display.drawFastVLine(x, y, h, color);
    }
}

void DisplayAtomS3R::set_text_color(uint16_t fg, uint16_t bg) {
    text_fg_ = fg;
    text_bg_ = bg;
    M5.Display.setTextColor(fg, bg);
}
void DisplayAtomS3R::set_text_size(uint8_t size) {
    text_size_ = size;
    M5.Display.setTextSize(size);
}
void DisplayAtomS3R::draw_text(int x, int y, const char* text) {
    if (buffered_) {
        buffer_sprite_.setTextColor(text_fg_, text_bg_);
        buffer_sprite_.setTextSize(text_size_);
        buffer_sprite_.setCursor(x - buffer_x_, y - buffer_y_);
        buffer_sprite_.print(text ? text : "");
    } else {
        M5.Display.setCursor(x, y);
        M5.Display.print(text ? text : "");
    }
}

void DisplayAtomS3R::set_text_wrap(bool wrap_x, bool wrap_y) {
    M5.Display.setTextWrap(wrap_x, wrap_y);
    if (buffered_) {
        buffer_sprite_.setTextWrap(wrap_x, wrap_y);
    }
}

void DisplayAtomS3R::flush() {
    // No-op: direct backend, no buffering. Buffered paint sessions push
    // explicitly via end_buffered_paint().
}

bool DisplayAtomS3R::begin_buffered_paint(int x, int y, int w, int h) {
    if (buffered_) end_buffered_paint();

    buffer_sprite_.setColorDepth(16);
    if (!buffer_sprite_.createSprite(w, h)) {
        return false;
    }
    buffer_x_ = x;
    buffer_y_ = y;
    buffer_w_ = w;
    buffer_h_ = h;
    buffered_ = true;
    return true;
}

void DisplayAtomS3R::end_buffered_paint() {
    if (!buffered_) return;
    buffer_sprite_.pushSprite(buffer_x_, buffer_y_);
    buffer_sprite_.deleteSprite();
    buffered_ = false;
}

}  // namespace hal
}  // namespace nocturnation

#endif  // NOCT_ATOMS3R
