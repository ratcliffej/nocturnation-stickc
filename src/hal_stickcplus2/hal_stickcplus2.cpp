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
//
// Newly declared (Epic 4 Block 3):
//
//   ESPNow  - WiFi-radio broadcast/peer messaging. begin() is NOT called
//             from HAL::begin(); orchestration brings the radio up when
//             entering a mode that needs it (Director as Director,
//             Lume Mode for receive). Mirrors the Mic on-demand pattern.

#include "hal/hal.h"
#include "M5Unified.h"

#include "display_stickcplus2.h"
#include "buttons_stickcplus2.h"
#include "imu_stickcplus2.h"
#include "battery_stickcplus2.h"
#include "mic_stickcplus2.h"
#include "ir_tx_stickcplus2.h"
#include "esp_now_stickcplus2.h"
#include "led_strip_stickcplus2.h"

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
    Capability::ESPNow,
    Capability::Bluetooth,   // BLE 4.2 hardware; declaration only - future
                             // Epic 4+ work wires phone-app pairing on top.

    // Audio analyser sub-capabilities lit by Epic 4.5. Plus2 and S3
    // declare the same set; they differ only in available CPU headroom
    // (S3 has esp-dsp HW-accelerated FFT) and in the size of their
    // declared audio_pipeline_operating_points list.
    Capability::AnalyserBeatDetection,
    Capability::AnalyserDropDetection,
    Capability::AnalyserSpectrumFrame,
    Capability::AnalyserBandSummary,

    // Epic 7: the USB-CDC peripheral can carry an Enttec Pro DMX
    // stream at 921 600 baud while DmxBridge mode is active. Mode-
    // entry switches Serial off the 115 200 console rate; mode-exit
    // restores it.
    Capability::DmxInput,

    // Epic 12 live B4: Grove-connected SK6812 flex strip on GPIO 33.
    // Always declared - the LedStripDriver writes bits regardless of
    // whether a strip is plugged in (the data line just radiates to
    // a dead pin). A future Config "LED Strip -> Enable" toggle can
    // gate the driver at runtime once that wiring lands.
    Capability::LedStrip,

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
DisplayStickCplus2 s_display;
ButtonsStickCplus2 s_buttons;
IMUStickCplus2     s_imu;
BatteryStickCplus2 s_battery;
MicStickCplus2     s_mic;
IRTxStickCplus2    s_ir_tx;       // built-in IR LED, GPIO 19
IRTxStickCplus2    s_ir_tx_ext(   // optional external IR unit, GPIO 26 header
    IRTxStickCplus2::kExternalIRPin);
ESPNowStickCplus2  s_esp_now;
LedStripStickCplus2 s_led_strip;
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
    // The external IR transmitter is begun unconditionally: IRsend::begin()
    // just configures GPIO 26 and an RMT channel, which is harmless when no
    // unit is plugged in. Whether it actually emits is gated at the driver
    // by the operator's external-IR toggle (default off).
    s_ir_tx_ext.begin();
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

Mic*     HAL::mic()      { return &s_mic; }
IRTx*    HAL::ir_tx()     { return &s_ir_tx; }
IRTx*    HAL::ir_tx_ext() { return &s_ir_tx_ext; }
ESPNow*  HAL::esp_now()  { return &s_esp_now; }
Display* HAL::display()  { return &s_display; }
Buttons* HAL::buttons()  { return &s_buttons; }
IMU*     HAL::imu()      { return &s_imu; }
Battery* HAL::battery()  { return &s_battery; }
LedStrip* HAL::led_strip() { return &s_led_strip; }
// Plus2 has battery + USB-C supply. Operator-cycle resolution is
// sufficient; no firmware cap.
uint8_t HAL::max_strip_brightness_percent() { return 100; }

}  // namespace hal
}  // namespace nocturnation
