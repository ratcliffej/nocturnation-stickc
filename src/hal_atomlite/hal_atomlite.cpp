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
//
// Capabilities deliberately NOT declared:
//
//   ESPNow    - the Atom has a radio but Phase 1 ships without wiring
//               an ESP-NOW backend here. Lume reception lands in a
//               follow-on block once the rest of the boot path is
//               bench-validated. Until then the Atom is an
//               output-only host - useful for bench-side LED testing
//               via direct USB-CDC commands (B2 driver seam) but not
//               yet field-deployable as a Lume.
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

namespace nocturnation {
namespace hal {

// -----------------------------------------------------------------------------
// Capability list
// -----------------------------------------------------------------------------

static constexpr Capability kCapabilities[] = {
    Capability::Buttons,
    Capability::LedStrip,
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
ESPNow*   HAL::esp_now()   { return nullptr; }
Display*  HAL::display()   { return nullptr; }
Buttons*  HAL::buttons()   { return &s_buttons; }
IMU*      HAL::imu()       { return nullptr; }
Battery*  HAL::battery()   { return nullptr; }
LedStrip* HAL::led_strip() { return &s_led_strip; }

}  // namespace hal
}  // namespace nocturnation
