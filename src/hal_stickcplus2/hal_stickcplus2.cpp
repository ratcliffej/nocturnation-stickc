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
// Capabilities currently declared (continued):
//
//   Mic       - PDM mic via I2S, FFT done by arduinoFFT. Emits AudioFrames
//               only while begin()/end() has been used to enable it.
//   IRTx      - GPIO 19 IR LED via IRremoteESP8266. Owns the only IRsend
//               instance in the firmware; main.cpp's previous global
//               IRsend has been removed.
//
// Not yet declared (interfaces exist; backends pending):
//
//   IRRx    - hardware exists on the Plus2; not used in Epic 2.
//   ESPNow  - Epic 4.

#include "hal/hal.h"
#include "M5Unified.h"

#include "display_stickcplus2.h"
#include "buttons_stickcplus2.h"
#include "imu_stickcplus2.h"
#include "battery_stickcplus2.h"
#include "mic_stickcplus2.h"
#include "ir_tx_stickcplus2.h"

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
    Capability::Mic,
    Capability::IRTx,
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
DisplayStickCplus2 s_display;
ButtonsStickCplus2 s_buttons;
IMUStickCplus2     s_imu;
BatteryStickCplus2 s_battery;
MicStickCplus2     s_mic;
IRTxStickCplus2    s_ir_tx;
}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin() {
    // Framework startup: bring up the M5Unified runtime before any backend
    // begin() runs. The bulk of peripheral init (display panel, AXP192 power,
    // BtnA/BtnB/BtnPWR) happens inside M5.begin() so subsequent backend
    // begin() calls can rely on hardware being live.
    auto cfg = M5.config();
    M5.begin(cfg);

    s_display.begin();
    s_buttons.begin();
    s_imu.begin();
    // Battery has no begin() in the HAL interface - M5.Power is up after
    // M5.begin() and the BatteryStickCplus2 backend is purely a query wrapper.
    // Mic is NOT begun here. Orchestration enables it via the DAL
    // (DAL::start_audio_input) when entering beat mode and disables it
    // when leaving; the prototype's M5.Mic / M5.Speaker contention pattern
    // is preserved exactly.
    s_ir_tx.begin();
}

void HAL::loop_tick() {
    // Refresh M5Unified per-frame state (button edges, etc.) before any
    // polled backend reads from it. ButtonsStickCplus2::poll() depends on
    // this having been called.
    M5.update();

    // Polled capabilities advance here. Buttons emits events to subscribers
    // when M5Unified's edge state shows a transition. Mic produces an
    // AudioFrame on each successful M5.Mic.record() call (~32 ms blocking
    // window when running; cheap no-op when not).
    s_buttons.poll();
    s_mic.poll();
}

// -----------------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------------

IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }

Mic*     HAL::mic()      { return &s_mic; }
IRTx*    HAL::ir_tx()    { return &s_ir_tx; }
Display* HAL::display()  { return &s_display; }
Buttons* HAL::buttons()  { return &s_buttons; }
IMU*     HAL::imu()      { return &s_imu; }
Battery* HAL::battery()  { return &s_battery; }

}  // namespace hal
}  // namespace nocturnation
