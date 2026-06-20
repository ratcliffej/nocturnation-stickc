#include "led_strip_stickcplus2.h"

namespace nocturnation {
namespace hal {

void LedStripStickCplus2::begin() {
    if (begun_) return;
#ifdef ARDUINO
    grove_.begin();
    grove_.setBrightness(255);   // LedStripDriver applies the device cap
#endif
    begun_ = true;
}

size_t LedStripStickCplus2::pixel_count() const { return kGroveStripPixelCount; }

void LedStripStickCplus2::set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= kGroveStripPixelCount) return;
#ifdef ARDUINO
    grove_.setPixelColor(index, grove_.Color(r, g, b));
#else
    (void)r; (void)g; (void)b;
#endif
}

void LedStripStickCplus2::clear() {
#ifdef ARDUINO
    grove_.clear();
#endif
}

void LedStripStickCplus2::show() {
#ifdef ARDUINO
    grove_.show();
#endif
}

}  // namespace hal
}  // namespace nocturnation
