// M5StickC Plus2 LedStrip backend (Epic 12 live B4 follow-on).
//
// Drives the Grove-connected SK6812 flex strip. Plus2 has no onboard
// addressable LED of its own (the front LCD is a separate output
// owned by LocalDisplayBinding), so this backend is Grove-only:
// kGroveStripPixelCount pixels on GPIO 33 - the WHITE wire of the
// HY2.0-4P Grove connector. Matches the convention bench-verified
// on the Atom Lite (white = data for the M5Stack SK6812 flex-strip
// SKU shipping with this project).
//
// Driven by Adafruit_NeoPixel (lib_deps in platformio.ini). If no
// strip is plugged in the show() bursts are radiated to G33 with
// no listener - harmless.

#pragma once

#include "hal/hal.h"

#ifdef ARDUINO
#include <Adafruit_NeoPixel.h>
#endif

namespace nocturnation {
namespace hal {

class LedStripStickCplus2 : public LedStrip {
public:
    // Grove white wire on the M5StickC Plus2 = GPIO 33.
    static constexpr uint8_t kGrovePin = 33;
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
