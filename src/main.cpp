// NocturNation StickC firmware - top-level entry point.
//
// All operating behaviour now lives below this file:
//   - Hardware access goes through hal::HAL (StickC backend in src/hal_stickc/)
//   - Devices, drivers, and event dispatch go through dal::DAL (src/dal/)
//   - Mode FSM + per-mode UI/audio/IR handling goes through
//     modes::ModeMachine (src/modes/), which subscribes to DAL events at
//     begin() and routes them to the active mode.
//
// setup() and loop() are intentionally tiny: bring up the DAL, bring up the
// mode FSM, then advance both each tick.

#include "dal/dal.h"
#include "modes/mode_machine.h"

void setup() {
    nocturnation::dal::DAL::begin();
    nocturnation::modes::ModeMachine::begin();
}

void loop() {
    nocturnation::dal::DAL::loop_tick();
    nocturnation::modes::ModeMachine::loop_tick();
}
