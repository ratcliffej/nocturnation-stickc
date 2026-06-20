// M5StickS3 LedStrip backend (Epic 12 live B4 follow-on).
//
// Sister backend to LedStripStickCplus2. Drives the Grove-connected
// SK6812 flex strip on GPIO 10 - the WHITE wire of the HY2.0-4P
// Grove connector on the StickS3 (G10 here vs G33 on the Plus2;
// the S3's pin map is different but the wire convention is the
// same).

#pragma once

#include "hal/hal.h"

#ifdef ARDUINO
#include <Adafruit_NeoPixel.h>
#endif

namespace nocturnation {
namespace hal {

class LedStripStickCS3 : public LedStrip {
public:
    // Grove white wire on the M5StickS3 = GPIO 10.
    static constexpr uint8_t kGrovePin = 10;
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
