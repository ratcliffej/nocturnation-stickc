// NocturNation HAL - M5StickC Plus2 backend.
//
// Declares the host's available capabilities and returns concrete backend
// instances from each accessor. Capabilities currently declared:
//
//   Display   - LovyanGFX behind M5.Display
//   Buttons   - 3 buttons (BtnA -> Btn1, BtnB -> Btn2, BtnPWR -> Btn3)
//   IMU       - MPU6886
//   Battery   - AXP192
//
// Not yet declared (interfaces exist; backends pending):
//
//   Mic     - lifecycle clashes with main.cpp's setBeatMode()/M5.Mic.begin/end
//             pattern; will land when beat detection migrates to orchestration.
//   IRTx    - main.cpp owns the global IRsend(IR_PIN); declaring IRTx here
//             would mean a second IRsend instance fighting for GPIO 19's RMT
//             channel. Will land when IRsend ownership consolidates into the
//             HAL backend (and main.cpp's irsend.sendRaw call sites migrate
//             to DAL helpers).
//   IRRx    - hardware exists on the Plus2; not used in Epic 2.
//   ESPNow  - Epic 4.

#include "hal/hal.h"

#include "display_stickc.h"
#include "buttons_stickc.h"
#include "imu_stickc.h"
#include "battery_stickc.h"

namespace nocturnation {
namespace hal {

// -----------------------------------------------------------------------------
// Capability list - declares what this backend offers
// -----------------------------------------------------------------------------

static constexpr Capability kCapabilities[] = {
    Capability::Display,
    Capability::Buttons,
    Capability::IMU,
    Capability::Battery,
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
// Backend instances (static singletons)
// -----------------------------------------------------------------------------

namespace {
DisplayStickC s_display;
ButtonsStickC s_buttons;
IMUStickC     s_imu;
BatteryStickC s_battery;
}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin() {
    // M5.begin() in main.cpp performs the bulk of hardware init; backend
    // begin() methods are no-ops on this host (every M5Unified-managed
    // peripheral is already up by the time we get here). The calls remain
    // for symmetry and to give per-capability backends a hook.
    s_display.begin();
    s_buttons.begin();
    s_imu.begin();
    // Battery has no begin() in the HAL interface - M5.Power is up after
    // M5.begin() and the BatteryStickC backend is purely a query wrapper.
}

void HAL::loop_tick() {
    // Polled capabilities advance here. Buttons emits events to subscribers
    // when M5Unified's edge state shows a transition. (Mic frame production
    // will land here when the Mic backend ships.)
    s_buttons.poll();
}

// -----------------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------------

Mic*     HAL::mic()      { return nullptr; }   // not yet implemented
IRTx*    HAL::ir_tx()    { return nullptr; }   // not yet implemented
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }

Display* HAL::display()  { return &s_display; }
Buttons* HAL::buttons()  { return &s_buttons; }
IMU*     HAL::imu()      { return &s_imu; }
Battery* HAL::battery()  { return &s_battery; }

}  // namespace hal
}  // namespace nocturnation
