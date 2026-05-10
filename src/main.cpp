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
#include "visualisations/visualisation_registry.h"
#include "visualisations/beat_pulse.h"
#include "visualisations/spectrum_bars.h"
#include "output_bindings/output_binding_registry.h"
#include "output_bindings/local_display.h"
#include "output_bindings/pixmob_ir.h"

void setup() {
    // Bring up the USB-CDC / UART console. Required before any Serial.printf
    // output flushes (arduino-esp32's CDC-on-boot path auto-creates the
    // peripheral but doesn't open the stream until begin() is called). 115200
    // matches platformio.ini's monitor_speed.
    Serial.begin(115200);
    delay(50);                  // brief settle so the boot banner isn't lost
    Serial.println("[noct] boot");

    nocturnation::dal::DAL::begin();

    // Register master-side visualisations. BeatPulse landed in Block 8;
    // SpectrumBars in Block 11. Order matters for the picker UI:
    // BeatPulse first so it's the default cursor row on a fresh boot
    // before NVS has anything saved.
    nocturnation::visualisations::visualisation_registry().register_plugin(
        nocturnation::visualisations::beat_pulse_instance());
    nocturnation::visualisations::visualisation_registry().register_plugin(
        nocturnation::visualisations::spectrum_bars_instance());

    // Register slave-side output bindings (Epic 4.6 Block 9). SlaveMode
    // walks this registry on enter() and activates every binding whose
    // required capabilities the host supports. LocalDisplayBinding
    // owns the "local" render surface (screen); PixMobIrBinding owns
    // IR forward to bracelets in this slave's configured group.
    nocturnation::output_bindings::output_binding_registry().register_plugin(
        nocturnation::output_bindings::local_display_instance());
    nocturnation::output_bindings::output_binding_registry().register_plugin(
        nocturnation::output_bindings::pixmob_ir_instance());

    nocturnation::modes::ModeMachine::begin();
}

void loop() {
    nocturnation::dal::DAL::loop_tick();
    nocturnation::modes::ModeMachine::loop_tick();
}
