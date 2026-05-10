// NocturNation HAL - 2-button input action mapper.
//
// Stateful 2-button mapper. Construct once per host; feed every
// (ButtonId, ButtonEvent) the Buttons backend produces; the mapper
// emits InputEvents via the provided callback. State is internal.
//
// 2-button mapping (StickC Plus2 + S3):
//   Btn1 Clicked       -> Confirm
//   Btn2 Clicked       -> Cycle
//   Btn1 LongPressed   -> Settings
//   Btn2 LongPressed   -> Picker
//   Btn1 DoubleClicked -> Pause
//
// CyclePrev / AuxA / AuxB are not emitted on this layout - hosts with
// only two buttons can't reach them. Consumers that depend on those
// must degrade gracefully on this host.

#pragma once

#include "hal/hal.h"
#include "hal/input_action.h"

#include <functional>

namespace nocturnation {
namespace hal {

class InputActionMapper2Btn {
public:
    using Emit = std::function<void(const InputEvent&)>;

    explicit InputActionMapper2Btn(Emit emit);

    // Feed every button event. Internal state machine emits zero or
    // more InputEvents synchronously via the Emit callback.
    void on_button_event(ButtonId id, ButtonEvent kind, uint32_t now_ms);

private:
    Emit emit_;
};

}  // namespace hal
}  // namespace nocturnation
