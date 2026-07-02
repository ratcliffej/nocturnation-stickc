// M5Atom Lite Buttons backend (Epic 12 B1).
//
// The Atom Lite has one programmable button on GPIO 39. Active low when
// pressed. Wraps the digitalRead in an edge-detector + long-press timer
// that drives Pressed / Released / Clicked / LongPressed callbacks
// matching the StickC backends.
//
//   Btn1 -> front button on GPIO 39.
//
// poll() is called from HAL::loop_tick() at the per-frame cadence. The
// debounce + long-press windows are software-timed off millis().

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ButtonsAtomLite : public Buttons {
public:
    static constexpr uint8_t kFrontButtonPin = 39;

    void begin() override;
    uint8_t count() const override;
    void set_callback(ButtonCallback cb) override;
    bool is_pressed(ButtonId id) override;
    void set_long_press_ms(uint16_t ms) override;

    void poll();

private:
    ButtonCallback callback_;
    uint16_t       long_press_ms_      = 1000;
    bool           last_pressed_       = false;
    uint32_t       pressed_at_ms_      = 0;
    bool           long_press_fired_   = false;
};

}  // namespace hal
}  // namespace nocturnation
