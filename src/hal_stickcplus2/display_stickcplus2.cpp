#include "display_stickcplus2.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void DisplayStickCplus2::begin() {
    // Panel itself is initialised by HAL::begin()'s M5.begin() call earlier.
    // Apply the StickC Plus2's natural landscape orientation (BtnA on the
    // right). All UI coordinates assume this rotation.
    M5.Display.setRotation(1);
}

void DisplayStickCplus2::set_rotation(uint8_t rotation) { M5.Display.setRotation(rotation); }
int  DisplayStickCplus2::width() const                  { return M5.Display.width(); }
int  DisplayStickCplus2::height() const                 { return M5.Display.height(); }

void DisplayStickCplus2::clear(uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(0, 0, buffer_w_, buffer_h_, color);
    } else {
        M5.Display.fillScreen(color);
    }
}

void DisplayStickCplus2::fill_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.fillRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.fillRect(x, y, w, h, color);
    }
}

void DisplayStickCplus2::draw_rect(int x, int y, int w, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawRect(x - buffer_x_, y - buffer_y_, w, h, color);
    } else {
        M5.Display.drawRect(x, y, w, h, color);
    }
}

void DisplayStickCplus2::draw_hline(int x, int y, int w, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastHLine(x - buffer_x_, y - buffer_y_, w, color);
    } else {
        M5.Display.drawFastHLine(x, y, w, color);
    }
}

void DisplayStickCplus2::draw_vline(int x, int y, int h, uint16_t color) {
    if (buffered_) {
        buffer_sprite_.drawFastVLine(x - buffer_x_, y - buffer_y_, h, color);
    } else {
        M5.Display.drawFastVLine(x, y, h, color);
    }
}

void DisplayStickCplus2::set_text_color(uint16_t fg, uint16_t bg) {
    text_fg_ = fg;
    text_bg_ = bg;
    M5.Display.setTextColor(fg, bg);
}
void DisplayStickCplus2::set_text_size(uint8_t size) {
    text_size_ = size;
    M5.Display.setTextSize(size);
}
void DisplayStickCplus2::draw_text(int x, int y, const char* text) {
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

void DisplayStickCplus2::set_text_wrap(bool wrap_x, bool wrap_y) {
    // M5GFX defaults to (true, false). The marquee path in
    // LumeTextBinding sets (false, false) so over-width header text
    // clips rather than wrapping onto subsequent lines. Buffered
    // sprite needs the same setting because Print routes through it
    // during a buffered paint.
    M5.Display.setTextWrap(wrap_x, wrap_y);
    if (buffered_) {
        buffer_sprite_.setTextWrap(wrap_x, wrap_y);
    }
}

void DisplayStickCplus2::flush() {
    // No-op: direct backend, no buffering. Buffered paint sessions push
    // explicitly via end_buffered_paint().
}

bool DisplayStickCplus2::begin_buffered_paint(int x, int y, int w, int h) {
    // Nesting not supported - close any in-flight session first.
    if (buffered_) end_buffered_paint();

    buffer_sprite_.setColorDepth(16);
    if (!buffer_sprite_.createSprite(w, h)) {
        // Allocation failed (RAM tight). Caller's draw_* calls fall through
        // to direct M5.Display routes, which still works - just no batching.
        return false;
    }
    buffer_x_ = x;
    buffer_y_ = y;
    buffer_w_ = w;
    buffer_h_ = h;
    buffered_ = true;
    return true;
}

void DisplayStickCplus2::end_buffered_paint() {
    if (!buffered_) return;
    buffer_sprite_.pushSprite(buffer_x_, buffer_y_);
    buffer_sprite_.deleteSprite();
    buffered_ = false;
}

}  // namespace hal
}  // namespace nocturnation
