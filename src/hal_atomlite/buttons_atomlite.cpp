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
        // Press edge. Are we inside the double-tap window after a prior
        // Clicked? If so, mark this as the second tap - a subsequent
        // long-hold emits DoubleTapHeld instead of LongPressed.
        last_pressed_      = true;
        pressed_at_ms_     = now;
        long_press_fired_  = false;
        in_double_tap_second_ =
            (last_clicked_at_ms_ != 0) &&
            ((now - last_clicked_at_ms_) < kDoubleTapWindowMs);
        callback_(ButtonId::Btn1, ButtonEvent::Pressed);
    } else if (!pressed_now && last_pressed_) {
        // Release edge. Emit Released and (if short) Clicked.
        last_pressed_ = false;
        callback_(ButtonId::Btn1, ButtonEvent::Released);
        const uint32_t held_for = now - pressed_at_ms_;
        if (!long_press_fired_ && held_for < long_press_ms_) {
            callback_(ButtonId::Btn1, ButtonEvent::Clicked);
            // Only arm the double-tap window if this was the FIRST
            // click (not the second-tap release). Otherwise two taps
            // in quick succession would chain into a triple-tap arm.
            last_clicked_at_ms_ = in_double_tap_second_ ? 0 : now;
        } else {
            last_clicked_at_ms_ = 0;
        }
        in_double_tap_second_ = false;
    } else if (pressed_now && last_pressed_ && !long_press_fired_) {
        // Sustained press - fire LongPressed (or DoubleTapHeld if we're
        // in the second-tap-of-a-double-tap window) once at the
        // threshold so a hold gesture can be acted on while the button
        // is still down.
        if ((now - pressed_at_ms_) >= long_press_ms_) {
            long_press_fired_ = true;
            if (in_double_tap_second_) {
                callback_(ButtonId::Btn1, ButtonEvent::DoubleTapHeld);
                last_clicked_at_ms_ = 0;
                in_double_tap_second_ = false;
            } else {
                callback_(ButtonId::Btn1, ButtonEvent::LongPressed);
            }
        }
    }

    // Expire a stale double-tap window if the operator never followed
    // up on the first click.
    if (last_clicked_at_ms_ != 0 && !last_pressed_ &&
        (now - last_clicked_at_ms_) >= kDoubleTapWindowMs) {
        last_clicked_at_ms_ = 0;
    }
#endif
}

}  // namespace hal
}  // namespace nocturnation
