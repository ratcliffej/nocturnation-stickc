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
    M5.Display.fillScreen(color);
}
void DisplayStickCplus2::fill_rect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.fillRect(x, y, w, h, color);
}
void DisplayStickCplus2::draw_rect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.drawRect(x, y, w, h, color);
}
void DisplayStickCplus2::draw_hline(int x, int y, int w, uint16_t color) {
    M5.Display.drawFastHLine(x, y, w, color);
}
void DisplayStickCplus2::draw_vline(int x, int y, int h, uint16_t color) {
    M5.Display.drawFastVLine(x, y, h, color);
}

void DisplayStickCplus2::set_text_color(uint16_t fg, uint16_t bg) {
    M5.Display.setTextColor(fg, bg);
}
void DisplayStickCplus2::set_text_size(uint8_t size) {
    M5.Display.setTextSize(size);
}
void DisplayStickCplus2::draw_text(int x, int y, const char* text) {
    M5.Display.setCursor(x, y);
    M5.Display.print(text ? text : "");
}

void DisplayStickCplus2::flush() {
    // No-op: direct backend, no buffering.
}

}  // namespace hal
}  // namespace nocturnation
