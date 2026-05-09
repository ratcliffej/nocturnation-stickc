// NocturNation M5StickC Plus2 firmware - top-level entry point.
//
// All operating behaviour now lives below this file:
//   - Hardware access goes through hal::HAL (StickC Plus2 backend in
//     src/hal_stickcplus2/; future host backends, e.g. M5 StickS3, slot in
//     alongside as their own src/hal_<host>/ folder).
//   - Devices, drivers, and event dispatch go through dal::DAL (src/dal/)
//   - Mode FSM + per-mode UI/audio/IR handling goes through
//     modes::ModeMachine (src/modes/), which subscribes to DAL events at
//     begin() and routes them to the active mode.
//
// setup() and loop() are intentionally tiny: bring up the DAL, bring up the
// mode FSM, then advance both each tick.

#include <Arduino.h>

#include "dal/dal.h"
#include "modes/mode_machine.h"

void setup() {
    // Bring up the USB-CDC / UART console. Required before any Serial.printf
    // output flushes (arduino-esp32's CDC-on-boot path auto-creates the
    // peripheral but doesn't open the stream until begin() is called). 115200
    // matches platformio.ini's monitor_speed.
    Serial.begin(115200);
    delay(50);                  // brief settle so the boot banner isn't lost
    Serial.println("[noct] boot");

    nocturnation::dal::DAL::begin();
    nocturnation::modes::ModeMachine::begin();
}

void loop() {
    nocturnation::dal::DAL::loop_tick();
    nocturnation::modes::ModeMachine::loop_tick();
}
