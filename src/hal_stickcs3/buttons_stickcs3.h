// M5StickS3 Buttons backend.
//
// Wraps M5.BtnA, M5.BtnB. The StickS3 exposes:
//   Btn1 -> M5.BtnA   (front "fire" button, GPIO 11 / KEY1)
//   Btn2 -> M5.BtnB   (side button, GPIO 12 / KEY2)
//
// The power button is NOT surfaced as Btn3 on the S3 (unlike the Plus2's
// BtnPWR). The S3's PMIC handles the power button in hardware: short press
// triggers a hard reset, long press cuts power. The firmware never sees a
// release-edge event, so trying to use BtnPWR for UI interactions (mode
// toggle, etc.) is unworkable on this host. Orchestration that previously
// bound to Btn3 on the Plus2 needs an alternative input on the S3 - this
// is captured as a follow-up under Epic 4 Block 2.
//
// poll() is called from HAL::loop_tick(); it reads M5Unified's edge-detected
// state (which assumes M5.update() has been called this frame, as main.cpp
// already does) and fires the registered callback for each transition.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ButtonsStickCS3 : public Buttons {
public:
    void begin() override;
    uint8_t count() const override;
    void set_callback(ButtonCallback cb) override;
    bool is_pressed(ButtonId id) override;
    void set_long_press_ms(uint16_t ms) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcs3.cpp.
    void poll();

private:
    ButtonCallback callback_;
    uint16_t       long_press_ms_ = 1000;
};

}  // namespace hal
}  // namespace nocturnation
