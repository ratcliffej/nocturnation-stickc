// NocturNation HAL - M5StickS3 backend.
//
// Sister backend to hal_stickcplus2 for the project's future reference
// platform. PlatformIO has no dedicated board ID for the StickS3, so the
// `[env:m5stack-stickcs3]` env in platformio.ini uses esp32-s3-devkitc-1
// and lets M5Unified detect the host at runtime via its
// board_t::board_M5StickS3 path. Pin assignments per docs.m5stack.com:
//
//   IR Tx        : GPIO 46 (was GPIO 19 on the Plus2)
//   IR Rx        : GPIO 42 (S3-only; not yet implemented)
//   Display SPI  : SCK 40, MOSI 39, CS 41, DC 45, RST 21, BL 38
//   ES8311 codec : I2C SDA 47 / SCL 48; I2S BCLK 17 / LRCK 15 / MCLK 18
//                  / DOUT 14 / DIN 16
//   Buttons      : KEY1 GPIO 11 (BtnA), KEY2 GPIO 12 (BtnB), PWR via PMU
//   IMU          : BMI270 on the same I2C bus as the codec
//   Battery      : monitored via M5.Power
//
// Capabilities currently declared:
//
//   Display, Buttons, IMU, Battery, Mic (ES8311), IRTx, IRRx
//
// Not yet declared (interfaces exist; implementations pending):
//
//   ESPNow  - Block 3 onwards (Epic 4).

#include "hal/hal.h"
#include "M5Unified.h"

#include "display_stickcs3.h"
#include "buttons_stickcs3.h"
#include "imu_stickcs3.h"
#include "battery_stickcs3.h"
#include "mic_stickcs3.h"
#include "ir_tx_stickcs3.h"
#include "ir_rx_stickcs3.h"

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
    Capability::IRRx,
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
DisplayStickCS3 s_display;
ButtonsStickCS3 s_buttons;
IMUStickCS3     s_imu;
BatteryStickCS3 s_battery;
MicStickCS3     s_mic;
IRTxStickCS3    s_ir_tx;
IRRxStickCS3    s_ir_rx;
}  // namespace

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin() {
    auto cfg = M5.config();
    M5.begin(cfg);

    s_display.begin();
    s_buttons.begin();
    s_imu.begin();
    // Battery has no begin() in the HAL interface - M5.Power is up after
    // M5.begin() and BatteryStickCS3 is purely a query wrapper.
    // Mic and IR Rx are NOT begun here; orchestration enables each on
    // demand (Mic in beat mode, IR Rx when a future Epic needs it).
    s_ir_tx.begin();
}

void HAL::loop_tick() {
    M5.update();
    s_buttons.poll();
    s_mic.poll();
    s_ir_rx.poll();
}

// -----------------------------------------------------------------------------
// Accessors
// -----------------------------------------------------------------------------

ESPNow*  HAL::esp_now()  { return nullptr; }

Mic*     HAL::mic()      { return &s_mic; }
IRTx*    HAL::ir_tx()    { return &s_ir_tx; }
IRRx*    HAL::ir_rx()    { return &s_ir_rx; }
Display* HAL::display()  { return &s_display; }
Buttons* HAL::buttons()  { return &s_buttons; }
IMU*     HAL::imu()      { return &s_imu; }
Battery* HAL::battery()  { return &s_battery; }

}  // namespace hal
}  // namespace nocturnation
