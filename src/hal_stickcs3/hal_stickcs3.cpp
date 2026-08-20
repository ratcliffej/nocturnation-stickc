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
//   Display, Buttons, IMU, Battery, Mic (ES8311), IRTx, IRRx, ESPNow
//
// ESPNow's begin() is NOT called from HAL::begin(); orchestration brings
// the radio up when entering a mode that needs it (mirrors the Mic
// on-demand pattern).

#include "hal/hal.h"
#include "M5Unified.h"

#include "display_stickcs3.h"
#include "buttons_stickcs3.h"
#include "imu_stickcs3.h"
#include "battery_stickcs3.h"
#include "mic_stickcs3.h"
#include "ir_tx_stickcs3.h"
#include "ir_rx_stickcs3.h"
#include "esp_now_stickcs3.h"
#include "led_strip_stickcs3.h"

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
    Capability::ESPNow,
    Capability::Bluetooth,   // BLE 5.0 hardware; declaration only - future
                             // Epic 4+ work wires phone-app pairing on top.

    // Audio analyser sub-capabilities lit by Epic 4.5. Plus2 and S3
    // declare the same set; they differ only in available CPU headroom
    // (S3 has esp-dsp HW-accelerated FFT) and in the size of their
    // declared audio_pipeline_operating_points list.
    Capability::AnalyserBeatDetection,
    Capability::AnalyserDropDetection,
    Capability::AnalyserSpectrumFrame,
    Capability::AnalyserBandSummary,

    // Epic 7: the native USB-CDC peripheral (ARDUINO_USB_CDC_ON_BOOT)
    // can carry an Enttec Pro DMX stream at 921 600 baud while
    // DmxBridge mode is active. Mode-entry switches Serial off the
    // 115 200 console rate; mode-exit restores it.
    Capability::DmxInput,

    // Epic 12 live B4: Grove-connected SK6812 flex strip on GPIO 10.
    // Always declared - the LedStripDriver writes bits regardless of
    // whether a strip is plugged in.
    Capability::AddressableLeds,

    // Epic 13: the Stick's LCD can render orchestrator-pushed text
    // and bitmap content. Both surfaces share the same M5Unified
    // framebuffer; the LumeTextBinding + LumeBitmapBinding compose
    // text on top of bitmap as their layering policy.
    Capability::DisplayText,
    Capability::DisplayBitmap,
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
DisplayStickCS3  s_display;
ButtonsStickCS3  s_buttons;
IMUStickCS3      s_imu;
BatteryStickCS3  s_battery;
MicStickCS3      s_mic;
IRTxStickCS3     s_ir_tx;
IRRxStickCS3     s_ir_rx;
ESPNowStickCS3   s_esp_now;
LedStripStickCS3 s_led_strip;
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

Mic*     HAL::mic()      { return &s_mic; }
IRTx*    HAL::ir_tx()    { return &s_ir_tx; }
// No external IR transmitter on the S3: its GPIO 26-equivalent header pins
// differ from the Plus2's and an M5Stack IR unit plugged in directly drives
// a strapping pin that forces the chip into boot mode. External IR is a
// Plus2-only feature for now.
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()    { return &s_ir_rx; }
ESPNow*  HAL::esp_now()  { return &s_esp_now; }
Display* HAL::display()  { return &s_display; }
Buttons* HAL::buttons()  { return &s_buttons; }
IMU*     HAL::imu()      { return &s_imu; }
Battery* HAL::battery()  { return &s_battery; }
LedStrip* HAL::led_strip() { return &s_led_strip; }
// S3 has battery + USB-C supply, but a laptop USB-C port typically
// delivers only ~900 mA, and a 30-pixel SK6812 at 50 % draws ~900 mA
// on peak white before the ESP32-S3 module's own ~300 mA - reliable
// Cap at 50 % for S3 - matches the fleet ceiling raised on 2026-07-11
// once the M5Stack TypeC2Grove Unit (U151) Y adapter made external
// Grove Y USB power for the strip a first-class option. Config >
// Connectivity > LED Strip > Brightness exposes {5, 10, 25, 50}; the
// 25 and 50 tiers require the Y adapter (labelled in the menu).
// Internal-battery / device-USB power stays safe at 5-10 %.
uint8_t HAL::max_strip_brightness_percent() { return 50; }

}  // namespace hal
}  // namespace nocturnation
