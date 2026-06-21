#include "display_stickcs3.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void DisplayStickCS3::begin() {
    // Panel itself is initialised by HAL::begin()'s M5.begin() call earlier.
    // The StickS3 mounts the same 1.14" ST7789 panel as the Plus2 but in the
    // opposite physical orientation, so M5Unified's rotation 1 ends up 180
    // degrees off from the Plus2. Use rotation 3 here to put the UI in the
    // same buttons-on-right orientation orchestration code assumes.
    M5.Display.setRotation(3);
}

void DisplayStickCS3::set_rotation(uint8_t rotation) { M5.Display.setRotation(rotation); }
int  DisplayStickCS3::width() const                  { return M5.Display.width(); }
int  DisplayStickCS3::height() const                 { return M5.Display.height(); }

void DisplayStickCS3::clear(uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(0, 0, buffer_w_, buffer_h_, color);
    } else {
        M5.Display.fillScreen(color);
    }
}

void DisplayStickCS3::fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.fillRect(x, y, w, h, color);
    }
}

void DisplayStickCS3::draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.drawRect(x, y, w, h, color);
    }
}

void DisplayStickCS3::draw_hline(int x, int y, int w, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastHLine(x - buffer_x_, y - buffer_y_, w, color);
    } else {
        M5.Display.drawFastHLine(x, y, w, color);
    }
}

void DisplayStickCS3::draw_vline(int x, int y, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastVLine(x - buffer_x_, y - buffer_y_, h, color);
    } else {
        M5.Display.drawFastVLine(x, y, h, color);
    }
}

void DisplayStickCS3::set_text_color(uint16_t fg, uint16_t bg) {
    text_fg_ = fg;
    text_bg_ = bg;
    M5.Display.setTextColor(fg, bg);
}
void DisplayStickCS3::set_text_size(uint8_t size) {
    text_size_ = size;
    M5.Display.setTextSize(size);
}
void DisplayStickCS3::draw_text(int x, int y, const char* text) {
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

void DisplayStickCS3::set_text_wrap(bool wrap_x, bool wrap_y) {
    M5.Display.setTextWrap(wrap_x, wrap_y);
    if (buffered_) {
        buffer_sprite_.setTextWrap(wrap_x, wrap_y);
    }
}

void DisplayStickCS3::flush() {
    // No-op: direct backend, no buffering. Buffered paint sessions push
    // explicitly via end_buffered_paint().
}

bool DisplayStickCS3::begin_buffered_paint(int x, int y, int w, int h) {
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

void DisplayStickCS3::end_buffered_paint() {
    if (!buffered_) return;
    buffer_sprite_.pushSprite(buffer_x_, buffer_y_);
    buffer_sprite_.deleteSprite();
    buffered_ = false;
}

}  // namespace hal
}  // namespace nocturnation
