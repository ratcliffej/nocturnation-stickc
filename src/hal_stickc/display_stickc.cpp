#include "display_stickc.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void DisplayStickC::begin() {
    // Panel itself is initialised by HAL::begin()'s M5.begin() call earlier.
    // Apply the StickC Plus2's natural landscape orientation (BtnA on the
    // right). All UI coordinates assume this rotation.
    M5.Display.setRotation(1);
}

void DisplayStickC::set_rotation(uint8_t rotation) { M5.Display.setRotation(rotation); }
int  DisplayStickC::width() const                  { return M5.Display.width(); }
int  DisplayStickC::height() const                 { return M5.Display.height(); }

void DisplayStickC::clear(uint16_t color) {
    M5.Display.fillScreen(color);
}
void DisplayStickC::fill_rect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.fillRect(x, y, w, h, color);
}
void DisplayStickC::draw_rect(int x, int y, int w, int h, uint16_t color) {
    M5.Display.drawRect(x, y, w, h, color);
}
void DisplayStickC::draw_hline(int x, int y, int w, uint16_t color) {
    M5.Display.drawFastHLine(x, y, w, color);
}
void DisplayStickC::draw_vline(int x, int y, int h, uint16_t color) {
    M5.Display.drawFastVLine(x, y, h, color);
}

void DisplayStickC::set_text_color(uint16_t fg, uint16_t bg) {
    M5.Display.setTextColor(fg, bg);
}
void DisplayStickC::set_text_size(uint8_t size) {
    M5.Display.setTextSize(size);
}
void DisplayStickC::draw_text(int x, int y, const char* text) {
    M5.Display.setCursor(x, y);
    M5.Display.print(text ? text : "");
}

void DisplayStickC::flush() {
    // No-op: direct backend, no buffering.
}

}  // namespace hal
}  // namespace nocturnation
