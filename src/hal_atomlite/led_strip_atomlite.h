// M5Atom Lite LedStrip backend (Epic 12 B1).
//
// Exposes one logical strip combining the Atom's onboard WS2812C-2020
// (1 pixel on GPIO 27) and the Grove-connected SK6812 flex strip
// (kGroveStripPixelCount pixels on GPIO 26 - the WHITE wire / SDA-pin
// of the HY2.0-4P Grove connector). The M5 documentation for "Unit
// RGB LED" (a different SKU) says yellow=data; the SK6812 flex strip
// product the project uses actually wires data to the white pin,
// matching working ESPHome examples for this same SKU.
// Pixel 0 is the onboard LED; pixels 1..kGroveStripPixelCount are
// the Grove strip in data-direction order.
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
    // GPIO 26 = Grove HY2.0-4P white wire / SDA pin on the Atom Lite.
    // The SK6812 flex strip's pigtail-to-Grove cable lands data on the
    // white wire. Confirmed against working ESPHome configurations for
    // this same M5Stack flex-strip SKU (bench-verified 2026-06-20).
    static constexpr uint8_t kGrovePin   = 26;

    // The shop sells the strip as 20 cm / 29 LEDs. Phase 1 assumes one
    // strip plugged into the Grove port; an extension stub for two
    // strips daisy-chained off Grove would update this constant once
    // bench confirms the timing budget.
    static constexpr size_t kOnboardPixelCount    = 1;
    // Max Grove strip length the operator can configure via Config >
    // LED Strip > Chain Size. The Adafruit_NeoPixel object is sized
    // here at construction; LedStripDriver::set_pixel_count() resizes
    // at runtime via updateLength() so show() walks only the
    // configured count. 288 matches the largest M5Stack flex strip
    // SKU (2 m).
    static constexpr size_t kGroveStripMaxPixels   = 288;
    static constexpr size_t kGroveStripPixelCount  = 29;     // default
    static constexpr size_t kTotalPixelCount =
        kOnboardPixelCount + kGroveStripMaxPixels;

    void begin() override;
    size_t pixel_count() const override;
    void set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void clear() override;
    void show() override;
    void set_pixel_count(size_t n) override;

private:
#ifdef ARDUINO
    Adafruit_NeoPixel onboard_{kOnboardPixelCount,     kOnboardPin, NEO_GRB + NEO_KHZ800};
    Adafruit_NeoPixel grove_  {kGroveStripMaxPixels,   kGrovePin,   NEO_GRB + NEO_KHZ800};
#endif
    size_t grove_count_ = kGroveStripPixelCount;
    bool begun_ = false;
};

}  // namespace hal
}  // namespace nocturnation
