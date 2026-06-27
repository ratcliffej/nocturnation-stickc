#include "led_strip_stickcs3.h"

namespace nocturnation {
namespace hal {

void LedStripStickCS3::begin() {
    if (begun_) return;
#ifdef ARDUINO
    grove_.begin();
    grove_.setBrightness(255);
    grove_.updateLength(grove_count_);
#endif
    begun_ = true;
}

size_t LedStripStickCS3::pixel_count() const { return grove_count_; }

void LedStripStickCS3::set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= grove_count_) return;
#ifdef ARDUINO
    grove_.setPixelColor(index, grove_.Color(r, g, b));
#else
    (void)r; (void)g; (void)b;
#endif
}

void LedStripStickCS3::set_pixel_count(size_t n) {
    if (n < 1) n = 1;
    if (n > kGroveStripMaxPixels) n = kGroveStripMaxPixels;
    grove_count_ = n;
#ifdef ARDUINO
    if (begun_) grove_.updateLength(grove_count_);
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
