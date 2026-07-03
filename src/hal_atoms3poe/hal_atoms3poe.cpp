// NocturNation HAL - M5 AtomS3 (Lite / S3R) + Atomic PoE (W5500) backend.
//
// A dedicated DMX-Director target. It runs ONLY DmxBridge mode: it
// receives sACN / Art-Net over wired Ethernet (W5500 on the Atomic PoE
// base) and broadcasts the resulting LIGHT_PULSE / LIGHT_WASH events to
// the fleet over ESP-NOW. No mic, no shows, no buttons.
//
// Why this board is the clean "Director is the bridge" answer: the console
// link is wired Ethernet over SPI (W5500), which never touches the WiFi
// radio. ESP-NOW therefore keeps the radio to itself in WIFI_STA / no-AP
// mode, exactly as on the Sticks - so there is none of the WiFi/ESP-NOW
// channel-coexistence problem a WiFi-sACN host would face.
//
// Declared capabilities:
//   ESPNow    - fleet broadcast (the only output that matters here)
//   DmxInput  - "host can receive DMX"; gates DmxBridge availability in the
//               mode machine. The wire is Ethernet here rather than the
//               Sticks' USB-CDC, but the capability semantics are the same.
//   Display   - AtomS3R variant only (NOCT_ATOMS3R): the S3R carries a
//               0.85" 128x128 panel, which DmxBridge uses for a status
//               dashboard. The Lite build leaves this undeclared.
//
// Everything else (Buttons, Mic, IRTx/Rx, IMU, Battery, analyser sub-caps)
// is intentionally NOT declared, so the per-capability accessors return
// nullptr and ModeMachine gates out Director / Lume / Config / Test.

#include "hal/hal.h"
#include "M5Unified.h"

#include "esp_now_atoms3poe.h"
#include "ir_tx_atoms3poe.h"
#if defined(NOCT_ATOMS3R)
#include "display_atoms3r.h"
#endif
#include "dal/drivers/net_config.h"

namespace nocturnation {
namespace hal {

// -----------------------------------------------------------------------------
// Capability list
// -----------------------------------------------------------------------------

static constexpr Capability kCapabilities[] = {
    Capability::ESPNow,
    Capability::DmxInput,
    // External IR emitter on Grove G2. With IRTx declared, the DAL's
    // "ir-pixmob" loopback fires automatically on every render_fx("00:00"),
    // so the Director lights nearby PixMob bracelets as its own Lume - no
    // DmxBridge changes needed.
    Capability::IRTx,
#if defined(NOCT_ATOMS3R)
    // AtomS3R's 128x128 LCD. Declaring Display routes the DAL's "local"
    // fire_display_* surface to the panel; DmxBridge draws its dashboard
    // through it. DisplayText/DisplayBitmap stay undeclared - those gate
    // Lume-side output bindings this Director never activates.
    Capability::Display,
#endif
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
ESPNowAtomS3Poe s_esp_now;
IRTxAtomS3Poe   s_ir_tx;
#if defined(NOCT_ATOMS3R)
DisplayAtomS3R  s_display;
#endif
}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin() {
    // Board bring-up (power, clocks, the single onboard RGB status LED on
    // the Lite / the LCD on the S3R). The panel's dedicated SPI pins don't
    // overlap the W5500's bus (5/6/7/8) on the Atomic PoE base, and on the
    // Lite M5.begin() does not touch a display SPI bus at all.
    auto cfg = M5.config();
    M5.begin(cfg);
#if defined(NOCT_ATOMS3R)
    s_display.begin();
#endif
    s_ir_tx.begin();   // external IR emitter on Grove G2
    // Ethernet (W5500) + the sACN/Art-Net listeners are brought up lazily by
    // the EthernetDmxAdapter on DmxBridge entry, mirroring how the Sticks
    // bring up their USB-CDC DMX adapter on mode entry.
}

void HAL::loop_tick() {
    M5.update();
    // USB-serial config console. Non-blocking and a no-op unless a host is
    // connected, so it never interrupts the broadcast. DMX arrives over
    // Ethernet, leaving USB-CDC free for this.
    nocturnation::dal::net_console_poll();
}

// -----------------------------------------------------------------------------
// Accessors - ESP-NOW, IR, and (S3R only) the display; the rest are nullptr.
// -----------------------------------------------------------------------------

Mic*     HAL::mic()       { return nullptr; }
IRTx*    HAL::ir_tx()     { return &s_ir_tx; }
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()     { return nullptr; }
ESPNow*  HAL::esp_now()   { return &s_esp_now; }
#if defined(NOCT_ATOMS3R)
Display* HAL::display()   { return &s_display; }
#else
Display* HAL::display()   { return nullptr; }
#endif
Buttons* HAL::buttons()   { return nullptr; }
IMU*     HAL::imu()       { return nullptr; }
Battery* HAL::battery()   { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }   // no LED strip on this board

// Pre-existing rollup miss from 34950ae (Atom-Lite 10% strip-brightness
// HARD-CEILING): every HAL backend gained this symbol except this one,
// because AtomS3-PoE is headless and has no LED strip. dal.cpp:411 calls
// it unconditionally before checking led_strip()==nullptr, so the symbol
// must exist or the firmware won't link. Return 100 (no cap) since the
// value is never consumed in practice on this board.
uint8_t HAL::max_strip_brightness_percent() { return 100; }

}  // namespace hal
}  // namespace nocturnation
