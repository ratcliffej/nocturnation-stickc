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
    // Start at the default count; LumeMode applies the persisted
    // value via set_pixel_count() on enter().
    grove_.updateLength(grove_count_);
#endif
    begun_ = true;
}

size_t LedStripAtomLite::pixel_count() const {
    return kOnboardPixelCount + grove_count_;
}

void LedStripAtomLite::set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) {
    if (index >= kOnboardPixelCount + grove_count_) return;
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

void LedStripAtomLite::set_pixel_count(size_t n) {
    // Operator-configurable chain size only affects the Grove strip;
    // the onboard pixel is always exactly one pixel. Clamp to the
    // construction-time max so updateLength() can't grow past the
    // initial buffer allocation.
    if (n < kOnboardPixelCount) n = kOnboardPixelCount;
    size_t grove_n = n - kOnboardPixelCount;
    if (grove_n > kGroveStripMaxPixels) grove_n = kGroveStripMaxPixels;
    grove_count_ = grove_n;
#ifdef ARDUINO
    if (begun_) grove_.updateLength(grove_count_);
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
