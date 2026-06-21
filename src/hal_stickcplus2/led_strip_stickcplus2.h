// M5StickC Plus2 LedStrip backend (Epic 12 live B4 follow-on).
//
// Drives the Grove-connected SK6812 flex strip. Plus2 has no onboard
// addressable LED of its own (the front LCD is a separate output
// owned by LocalDisplayBinding), so this backend is Grove-only:
// kGroveStripPixelCount pixels on GPIO 32 - per M5Stack's own canonical
// example for the SK6812 RGB unit on M5StickC
// (m5stack/M5StickC/examples/Unit/RGB_SK6812/RGB_SK6812.ino), which
// hardwires PIN 32. The wire-colour convention diverges per board on
// the M5 stack: Atom Lite uses G26 (SDA-side), StickC Plus2 uses G32
// (SCL-side). Pin assignment is per-host, not a uniform Grove rule.
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
    // GPIO 32 = M5Stack's canonical SK6812 pin on the StickC family.
    static constexpr uint8_t kGrovePin            = 32;
    static constexpr size_t  kGroveStripMaxPixels  = 288;    // 2 m strip
    static constexpr size_t  kGroveStripPixelCount = 29;     // default

    void begin() override;
    size_t pixel_count() const override;
    void set_pixel(size_t index, uint8_t r, uint8_t g, uint8_t b) override;
    void clear() override;
    void show() override;
    void set_pixel_count(size_t n) override;

private:
#ifdef ARDUINO
    Adafruit_NeoPixel grove_{kGroveStripMaxPixels, kGrovePin, NEO_GRB + NEO_KHZ800};
#endif
    size_t grove_count_ = kGroveStripPixelCount;
    bool begun_ = false;
};

}  // namespace hal
}  // namespace nocturnation
