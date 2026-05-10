// NocturNation HAL - 2-button input action mapper implementation.
//
// Stateless mapping table - no multi-button gestures needed for the
// 2-button layout. We deliberately dropped the "Btn1+Btn2 hold for
// Back" gesture in favour of "Picker / Settings as a toggle"; menus
// and overlays handle their own back semantics.

#include "hal/input_action_mapper_2btn.h"

namespace nocturnation {
namespace hal {

InputActionMapper2Btn::InputActionMapper2Btn(Emit emit) : emit_(std::move(emit)) {}

void InputActionMapper2Btn::on_button_event(ButtonId id, ButtonEvent kind, uint32_t now_ms) {
    InputAction action = InputAction::None;
    if (id == ButtonId::Btn1) {
        switch (kind) {
            case ButtonEvent::Clicked:       action = InputAction::Confirm;  break;
            case ButtonEvent::LongPressed:   action = InputAction::Settings; break;
            case ButtonEvent::DoubleClicked: action = InputAction::Pause;    break;
            default: break;
        }
    } else if (id == ButtonId::Btn2) {
        switch (kind) {
            case ButtonEvent::Clicked:     action = InputAction::Cycle;  break;
            case ButtonEvent::LongPressed: action = InputAction::Picker; break;
            default: break;
        }
    }
    if (action != InputAction::None && emit_) {
        emit_({action, now_ms});
    }
}

}  // namespace hal
}  // namespace nocturnation
