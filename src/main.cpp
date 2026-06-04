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
#include "shows/show_registry.h"
#include "shows/simple_beat_show.h"
#include "shows/dynamic_show.h"
#include "shows/wash_demo_show.h"
#include "shows/bass_and_drift_show.h"
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

    // Register Director-side Shows (Epic 4.7 Block 1). SimpleBeatShow
    // preserves the pre-Block-1 BeatPulse behaviour; Block 5 adds
    // DynamicShow. The pre-Block-2 BeatPulse / SpectrumBars Visualisation
    // registrations retired in Block 2 - their screen-rendering logic
    // moved into the widget library (src/widgets/) and SimpleBeatShow
    // composes BeatBarWidget for the flux meter; ConfigMode > Utilities
    // > Level Tuning hosts the widgets standalone for bench work.
    nocturnation::shows::show_registry().register_plugin(
        nocturnation::shows::simple_beat_show_instance());
    nocturnation::shows::show_registry().register_plugin(
        nocturnation::shows::dynamic_show_instance());
    // Epic 6C Phase E: integration-test Show for the WASH-family API.
    // Not a user-facing show; exists to exercise render_wash / render_fx
    // overlay / render_wash_end end-to-end against the Phase F renderer.
    nocturnation::shows::show_registry().register_plugin(
        nocturnation::shows::wash_demo_show_instance());
    // Epic 6D B2: reference reactive Show. The section/phrase/beat
    // three-timescale model from architecture.md §1.2, with manual drop
    // override on Btn1 and palette cycling on Btn2.
    nocturnation::shows::show_registry().register_plugin(
        nocturnation::shows::bass_and_drift_show_instance());

    // Register Lume-side output bindings (Epic 4.6 Block 9). LumeMode
    // walks this registry on enter() and activates every binding whose
    // required capabilities the host supports. LocalDisplayBinding
    // owns the "local" render surface (screen); PixMobIrBinding owns
    // IR forward to bracelets in this Lume's configured group.
    nocturnation::output_bindings::output_binding_registry().register_plugin(
        nocturnation::output_bindings::local_display_instance());
    nocturnation::output_bindings::output_binding_registry().register_plugin(
        nocturnation::output_bindings::pixmob_ir_instance());

    nocturnation::modes::ModeMachine::begin();
}

void loop() {
    nocturnation::dal::DAL::loop_tick();
    nocturnation::modes::ModeMachine::loop_tick();
    // Yield to the FreeRTOS scheduler. Without this delay the Arduino
    // loop runs at hundreds of kHz, pinning the CPU at full clock and
    // burning baseline current that the chip would otherwise spend in
    // the idle task's light-sleep window. delay(1) sleeps for one
    // FreeRTOS tick (~1 ms on the default tick rate), which is the
    // shortest yield the Arduino framework exposes and well below the
    // shortest cadence any mode needs (audio FFT is ~30 Hz, ESP-NOW
    // RX is interrupt-driven, LIGHT_PULSE fires top out at ~8 Hz).
    // Cuts roughly an order of magnitude off the loop frequency and
    // a meaningful chunk off baseline current draw - particularly
    // load-bearing for Lume mode on battery (spec v0.29 §8.2). All
    // modes benefit, including Director; Director's mains/USB power
    // budget makes the saving incidental there but not harmful.
    delay(1);
}
