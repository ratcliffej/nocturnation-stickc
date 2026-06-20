#include "led_strip_stickcs3.h"

namespace nocturnation {
namespace hal {

void LedStripStickCS3::begin() {
    if (begun_) return;
#ifdef ARDUINO
    grove_.begin();
    grove_.setBrightness(255);   // LedStripDriver applies the device cap
#endif
    begun_ = true;
}

size_t LedStripStickCS3::pixel_count() const { return kGroveStripPixelCount; }

void LedStripStickCS3::set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= kGroveStripPixelCount) return;
#ifdef ARDUINO
    grove_.setPixelColor(index, grove_.Color(r, g, b));
#else
    (void)r; (void)g; (void)b;
#endif
}

void LedStripStickCS3::clear() {
#ifdef ARDUINO
    grove_.clear();
#endif
}

void LedStripStickCS3::show() {
#ifdef ARDUINO
    grove_.show();
#endif
}

}  // namespace hal
}  // namespace nocturnation
