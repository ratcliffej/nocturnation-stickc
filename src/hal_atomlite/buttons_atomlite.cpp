#include "buttons_atomlite.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace hal {

void ButtonsAtomLite::begin() {
#ifdef ARDUINO
    pinMode(kFrontButtonPin, INPUT_PULLUP);
#endif
}

uint8_t ButtonsAtomLite::count() const { return 1; }

void ButtonsAtomLite::set_callback(ButtonCallback cb) { callback_ = cb; }

void ButtonsAtomLite::set_long_press_ms(uint16_t ms) { long_press_ms_ = ms; }

bool ButtonsAtomLite::is_pressed(ButtonId id) {
    if (id != ButtonId::Btn1) return false;
#ifdef ARDUINO
    return digitalRead(kFrontButtonPin) == LOW;
#else
    return false;
#endif
}

void ButtonsAtomLite::poll() {
    if (!callback_) return;
#ifdef ARDUINO
    const bool pressed_now = (digitalRead(kFrontButtonPin) == LOW);
    const uint32_t now = ::millis();

    if (pressed_now && !last_pressed_) {
        // Press edge.
        last_pressed_      = true;
        pressed_at_ms_     = now;
        long_press_fired_  = false;
        callback_(ButtonId::Btn1, ButtonEvent::Pressed);
    } else if (!pressed_now && last_pressed_) {
        // Release edge. Emit Released and (if short) Clicked.
        last_pressed_ = false;
        callback_(ButtonId::Btn1, ButtonEvent::Released);
        const uint32_t held_for = now - pressed_at_ms_;
        if (!long_press_fired_ && held_for < long_press_ms_) {
            callback_(ButtonId::Btn1, ButtonEvent::Clicked);
        }
    } else if (pressed_now && last_pressed_ && !long_press_fired_) {
        // Sustained press - fire LongPressed once at the threshold so a
        // hold gesture can be acted on while the button is still down
        // (matches M5Unified's wasHold() semantic the StickC backends use).
        if ((now - pressed_at_ms_) >= long_press_ms_) {
            long_press_fired_ = true;
            callback_(ButtonId::Btn1, ButtonEvent::LongPressed);
        }
    }
#endif
}

}  // namespace hal
}  // namespace nocturnation
