// M5Atom Lite LedStrip backend (Epic 12 B1).
//
// Exposes one logical strip combining the Atom's onboard WS2812C-2020
// (1 pixel on GPIO 27) and the Grove-connected SK6812 flex strip
// (kGroveStripPixelCount pixels on GPIO 32 - the YELLOW wire of the
// HY2.0-4P Grove connector, which is the M5Stack-canonical data line
// for their NeoPixel-class Grove units; verified against docs.m5stack.com
// Unit RGB LED page). Pixel 0 is the onboard LED; pixels
// 1..kGroveStripPixelCount are the Grove strip in data-direction order.
//
// Driven by Adafruit_NeoPixel (lib_deps in platformio.ini). One
// Adafruit_NeoPixel instance per physical pin so the show() bursts hit
// the right output. Both share the standard WS2812/SK6812 800 kHz
// timing - if a future board uses RGBW strips the constructor flag
// changes to NEO_GRBW.
//
// Phase 1 wiring assumes the Grove "white" wire (SDA / GPIO 26) carries
// the data line and the strip is fed +5V from Grove. Bench validation
// in Epic 12 B7 will confirm this; if it's wrong the pin assignment
// here is the single source of truth.

#pragma once

#include "hal/hal.h"

#ifdef ARDUINO
#include <Adafruit_NeoPixel.h>
#endif

namespace nocturnation {
namespace hal {

class LedStripAtomLite : public LedStrip {
public:
    static constexpr uint8_t kOnboardPin = 27;
    // GPIO 32 = Grove HY2.0-4P yellow wire on the Atom Lite. M5Stack's
    // NeoPixel-class Grove units (incl. the SK6812 flex strip used here)
    // carry the one-wire data signal on the yellow pin. Verified against
    // docs.m5stack.com/en/unit/neopixel + docs.m5stack.com/en/unit/rgb_led_strip
    // (2026-06-20).
    static constexpr uint8_t kGrovePin   = 32;

    // The shop sells the strip as 20 cm / 29 LEDs. Phase 1 assumes one
    // strip plugged into the Grove port; an extension stub for two
    // strips daisy-chained off Grove would update this constant once
    // bench confirms the timing budget.
    static constexpr size_t kOnboardPixelCount    = 1;
    static constexpr size_t kGroveStripPixelCount = 29;
    static constexpr size_t kTotalPixelCount =
        kOnboardPixelCount + kGroveStripPixelCount;

    void begin() override;
    size_t pixel_count() const override;
    void set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void clear() override;
    void show() override;

private:
#ifdef ARDUINO
    Adafruit_NeoPixel onboard_{kOnboardPixelCount,    kOnboardPin, NEO_GRB + NEO_KHZ800};
    Adafruit_NeoPixel grove_  {kGroveStripPixelCount, kGrovePin,   NEO_GRB + NEO_KHZ800};
#endif
    bool begun_ = false;
};

}  // namespace hal
}  // namespace nocturnation
