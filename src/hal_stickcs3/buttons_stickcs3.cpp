#include "buttons_stickcs3.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void ButtonsStickCS3::begin() {
    // No-op: M5.begin() in hal_stickcs3.cpp wires up the buttons.
}

uint8_t ButtonsStickCS3::count() const {
    return 2;   // BtnA (Btn1), BtnB (Btn2). Power button is hardware-managed.
}

void ButtonsStickCS3::set_callback(ButtonCallback cb) {
    callback_ = cb;
}

bool ButtonsStickCS3::is_pressed(ButtonId id) {
    switch (id) {
        case ButtonId::Btn1: return M5.BtnA.isPressed();
        case ButtonId::Btn2: return M5.BtnB.isPressed();
        default:             return false;
    }
}

void ButtonsStickCS3::set_long_press_ms(uint16_t ms) {
    long_press_ms_ = ms;
}

namespace {

template <typename Btn>
void check_button(ButtonsStickCS3::ButtonCallback const& cb,
                  ButtonId id, Btn& btn, uint16_t long_press_ms) {
    if (!cb) return;
    if (btn.wasPressed())                      cb(id, ButtonEvent::Pressed);
    if (btn.wasReleased())                     cb(id, ButtonEvent::Released);
    if (btn.wasClicked())                      cb(id, ButtonEvent::Clicked);
    if (btn.wasHold() ||
        (btn.isPressed() && btn.pressedFor(long_press_ms))) {
        cb(id, ButtonEvent::LongPressed);
    }
}

}  // namespace

void ButtonsStickCS3::poll() {
    if (!callback_) return;
    check_button(callback_, ButtonId::Btn1, M5.BtnA, long_press_ms_);
    check_button(callback_, ButtonId::Btn2, M5.BtnB, long_press_ms_);
}

}  // namespace hal
}  // namespace nocturnation
