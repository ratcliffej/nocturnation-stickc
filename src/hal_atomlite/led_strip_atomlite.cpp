#include "led_strip_atomlite.h"

namespace nocturnation {
namespace hal {

void LedStripAtomLite::begin() {
    if (begun_) return;
#ifdef ARDUINO
    onboard_.begin();
    grove_.begin();
    onboard_.setBrightness(255);
    grove_.setBrightness(255);
#endif
    begun_ = true;
}

size_t LedStripAtomLite::pixel_count() const { return kTotalPixelCount; }

void LedStripAtomLite::set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= kTotalPixelCount) return;
#ifdef ARDUINO
    if (index < kOnboardPixelCount) {
        onboard_.setPixelColor(index, onboard_.Color(r, g, b));
    } else {
        grove_.setPixelColor(index - kOnboardPixelCount, grove_.Color(r, g, b));
    }
#else
    (void)r; (void)g; (void)b;
#endif
}

void LedStripAtomLite::clear() {
#ifdef ARDUINO
    onboard_.clear();
    grove_.clear();
#endif
}

void LedStripAtomLite::show() {
#ifdef ARDUINO
    // Push onboard first - the user's eye is on it as the signal-state
    // indicator (Epic 12 B5) and the Grove strip's longer burst would
    // otherwise delay onboard updates by ~700 us.
    onboard_.show();
    grove_.show();
#endif
}

}  // namespace hal
}  // namespace nocturnation
