// NocturNation HAL - M5Atom Lite backend (Epic 12 B1).
//
// The lightest host the project ships. ESP32 PICO (single core), 1
// programmable front button (GPIO 39), 1 onboard WS2812C-2020 LED
// (GPIO 27), 1 Grove port (GPIO 26 data / GPIO 32 reserved), no
// display, no microphone, no IMU, no IR, no codec. Optional 200 mAh
// battery base for the field-deployable Lume gesture.
//
// Capabilities declared:
//
//   Buttons   - 1 front button (Btn1), active-low on GPIO 39.
//   LedStrip  - onboard pixel + Grove strip exposed as one
//               contiguous addressable strip (1 + 29 = 30 pixels).
//   ESPNow    - WiFi-radio broadcast/peer messaging. begin() is NOT
//               called from HAL::begin(); LumeMode brings the radio
//               up on entering Lume + auto-scans channels 11 / 1 /
//               6 (spec §5.3) until a Director frame locks the
//               channel. Implementation is a port of the Plus2
//               backend (board-agnostic Arduino-ESP32 + ESP-IDF
//               calls).
//
// PlatformIO env: [env:m5stack-atomlite] in platformio.ini. Board id
// `m5stack-atom` is shared between the Atom Lite and the Atom Matrix
// (the Matrix has 25 LEDs in a 5x5 grid wired off a single pin; this
// backend would need a matrix-aware variant for that host).

#include "hal/hal.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

#include "buttons_atomlite.h"
#include "led_strip_atomlite.h"
#include "esp_now_atomlite.h"

namespace nocturnation {
namespace hal {

// -----------------------------------------------------------------------------
// Capability list
// -----------------------------------------------------------------------------

static constexpr Capability kCapabilities[] = {
    Capability::Buttons,
    Capability::LedStrip,
    Capability::ESPNow,
};
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()    { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }

bool HAL::has(Capability c) {
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (kCapabilities[i] == c) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Backend instances
// -----------------------------------------------------------------------------

namespace {
ButtonsAtomLite  s_buttons;
LedStripAtomLite s_led_strip;
ESPNowAtomLite   s_esp_now;
}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin() {
    s_buttons.begin();
    // LedStrip's begin() is idempotent and gets called again by
    // LedStripDriver::begin() during DAL::begin(). Calling it here too
    // would set the strip up before the driver claims it; that's fine
    // (begin() is idempotent) but also unnecessary, so we leave it to
    // the driver.
}

void HAL::loop_tick() {
    s_buttons.poll();
}

// -----------------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------------

Mic*      HAL::mic()       { return nullptr; }
IRTx*     HAL::ir_tx()     { return nullptr; }
IRTx*     HAL::ir_tx_ext() { return nullptr; }
IRRx*     HAL::ir_rx()     { return nullptr; }
ESPNow*   HAL::esp_now()   { return &s_esp_now; }
Display*  HAL::display()   { return nullptr; }
Buttons*  HAL::buttons()   { return &s_buttons; }
IMU*      HAL::imu()       { return nullptr; }
Battery*  HAL::battery()   { return nullptr; }
LedStrip* HAL::led_strip() { return &s_led_strip; }
// Atom Lite power budget: 200 mAh battery base + 500 mA USB hub
// limit (when bench-tested off a hub). A 30-pixel SK6812 strip at
// full white draws ~60 mA / pixel = 1.8 A at 100 % brightness. At
// 25 % that's still ~450 mA, right at the USB-hub edge and routinely
// brownout-rebooting the chip. 10 % (~180 mA peak) fits inside every
// supply the Atom plausibly runs from, including the integrated
// battery base.
//
// Hard ceiling, not default - the LedStripDriver clamps any
// set_brightness_percent call above this. Operator can't push the
// Atom into brownout by mis-cycling the Btn1 brightness levels.
uint8_t HAL::max_strip_brightness_percent() { return 10; }

}  // namespace hal
}  // namespace nocturnation
