// M5StickS3 LedStrip backend (Epic 12 live B4 follow-on).
//
// Sister backend to LedStripStickCplus2. Drives the Grove-connected
// SK6812 flex strip on GPIO 9 - by symmetry with the Plus2's G32
// (M5's canonical pin for the SK6812 unit on StickC). G9 is the
// StickS3's Grove "yellow wire" / SCL-side pin, matching the Plus2's
// G32 role. Not bench-verified on the S3 yet; first S3 + strip
// bench session may need to swap to G10 if this turns out wrong.

#pragma once

#include "hal/hal.h"

#ifdef ARDUINO
#include <Adafruit_NeoPixel.h>
#endif

namespace nocturnation {
namespace hal {

class LedStripStickCS3 : public LedStrip {
public:
    // GPIO 9 = StickS3 Grove SCL-side pin (Plus2 symmetry; bench-pending).
    static constexpr uint8_t kGrovePin = 9;
    static constexpr size_t  kGroveStripPixelCount = 29;

    void begin() override;
    size_t pixel_count() const override;
    void set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void clear() override;
    void show() override;

private:
#ifdef ARDUINO
    Adafruit_NeoPixel grove_{kGroveStripPixelCount, kGrovePin, NEO_GRB + NEO_KHZ800};
#endif
    bool begun_ = false;
};

}  // namespace hal
}  // namespace nocturnation
