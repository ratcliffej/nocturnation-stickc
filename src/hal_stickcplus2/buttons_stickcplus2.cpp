#include "buttons_stickcplus2.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

void ButtonsStickCplus2::begin() {
    // No-op: M5.begin() in main.cpp wires up the buttons.
}

uint8_t ButtonsStickCplus2::count() const {
    return 3;   // BtnA (Btn1), BtnB (Btn2), BtnPWR (Btn3)
}

void ButtonsStickCplus2::set_callback(ButtonCallback cb) {
    callback_ = cb;
}

bool ButtonsStickCplus2::is_pressed(ButtonId id) {
    switch (id) {
        case ButtonId::Btn1: return M5.BtnA.isPressed();
        case ButtonId::Btn2: return M5.BtnB.isPressed();
        case ButtonId::Btn3: return M5.BtnPWR.isPressed();
        default:             return false;
    }
}

void ButtonsStickCplus2::set_long_press_ms(uint16_t ms) {
    long_press_ms_ = ms;
}

namespace {

// M5Unified Button_Class methods are idempotent within a frame (state is
// computed during M5.update() and cached until the next M5.update()), so it
// is safe for both this poll() and main.cpp's existing button checks to
// observe the same edge events.
template <typename Btn>
void check_button(ButtonsStickCplus2::ButtonCallback const& cb,
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

void ButtonsStickCplus2::poll() {
    if (!callback_) return;
    check_button(callback_, ButtonId::Btn1, M5.BtnA,   long_press_ms_);
    check_button(callback_, ButtonId::Btn2, M5.BtnB,   long_press_ms_);
    check_button(callback_, ButtonId::Btn3, M5.BtnPWR, long_press_ms_);
}

}  // namespace hal
}  // namespace nocturnation
