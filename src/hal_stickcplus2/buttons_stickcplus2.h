// M5StickC Plus2 Buttons backend.
//
// Wraps M5.BtnA and M5.BtnB only. The Plus2 has a third hardware button
// (BtnPWR / left-side power button) but we deliberately do NOT surface it,
// for cross-host UI consistency: the StickS3's PMIC owns the power button
// in hardware (short press = reset, long press = power off, neither
// reachable from firmware), so a two-button BtnA + BtnB UI is the only
// option that works identically on both hosts.
//
//   Btn1 -> M5.BtnA   (front "fire" button)
//   Btn2 -> M5.BtnB   (side button)
//
// poll() is called from HAL::loop_tick(); it reads M5Unified's edge-detected
// state (which assumes M5.update() has been called this frame, as main.cpp
// already does) and fires the registered callback for each transition.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ButtonsStickCplus2 : public Buttons {
public:
    void begin() override;
    uint8_t count() const override;
    void set_callback(ButtonCallback cb) override;
    bool is_pressed(ButtonId id) override;
    void set_long_press_ms(uint16_t ms) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcplus2.cpp.
    void poll();

private:
    ButtonCallback callback_;
    uint16_t       long_press_ms_ = 1000;
};

}  // namespace hal
}  // namespace nocturnation
