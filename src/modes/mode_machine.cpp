// NocturNation Mode FSM - facade implementation.
//
// This file holds the FSM facade only: the static singletons of each
// concrete mode, the active-mode pointer, the DAL event-routing
// callbacks, the public ModeMachine surface, and the native-build
// millis() seam (declared in include/modes/mode_machine.h).
//
// Concrete modes live in their own per-mode .h/.cpp pairs in this
// directory, and are pulled in via #include below so the static
// singletons below have complete types at the point of instantiation.
// Shared NVS persistence helpers live in modes/persistence.h.

#include "modes/mode_machine.h"           // public header in include/

#include "persistence.h"                  // co-located in src/modes/
#include "boot_mode.h"
#include "menu_mode.h"
#include "autonomous_master_mode.h"
#include "slave_mode.h"
#include "config_mode.h"
#include "test_mode.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_enabled gating

#ifdef ARDUINO
#include <Arduino.h>
#else
// Native test stubs. millis() is needed by the Boot countdown; tests advance
// it explicitly via the seam below.
#include <cstdint>
namespace {
uint32_t s_native_millis = 0;
}
extern "C" uint32_t millis() { return s_native_millis; }
namespace nocturnation {
namespace modes {
namespace test_seam {
void set_millis(uint32_t v) { s_native_millis = v; }
}  // namespace test_seam
}  // namespace modes
}  // namespace nocturnation
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;

// =============================================================================
// Internal state - active mode pointer + per-mode singletons.
// =============================================================================

namespace {

persistence::AudioCalibration s_calibration =
    persistence::kCalibrationDefault;

Mode*  s_active_mode      = nullptr;
ModeId s_last_runtime     = persistence::kDefaultRuntimeMode;

BootMode             s_boot;
MenuMode             s_menu;
AutonomousMasterMode s_autonomous_master;
SlaveMode            s_slave;
ConfigMode           s_config;
TestMode             s_test;

Mode* mode_instance(ModeId id) {
    switch (id) {
        case ModeId::Boot:             return &s_boot;
        case ModeId::Menu:             return &s_menu;
        case ModeId::AutonomousMaster: return &s_autonomous_master;
        case ModeId::Slave:            return &s_slave;
        case ModeId::Config:           return &s_config;
        case ModeId::Test:             return &s_test;
    }
    return nullptr;
}

void enter_mode(ModeId id) {
    Mode* next = mode_instance(id);
    if (!next) return;
    if (s_active_mode == next) return;
    if (s_active_mode) s_active_mode->exit();
    s_active_mode = next;
    if (persistence::is_persisted_runtime_mode(id)) {
        s_last_runtime = id;
        persistence::save_last_runtime_mode(id);
    }
    s_active_mode->enter();
}

void on_dal_button_press(const char*, const ButtonPressEvent& ev) {
    if (s_active_mode) s_active_mode->on_button_event(ev);
}

void on_dal_audio_frame(const char*, const AudioFrameEvent& ev) {
    if (s_active_mode) s_active_mode->on_audio_frame(ev);
}

}  // namespace

// =============================================================================
// Persistence accessors over the local statics.
// =============================================================================

namespace persistence {

AudioCalibration& current_calibration() {
    return modes::s_calibration;
}

ModeId current_last_runtime() {
    return modes::s_last_runtime;
}

}  // namespace persistence

// =============================================================================
// ModeMachine public API
// =============================================================================

void ModeMachine::begin() {
    DAL::subscribe_button_presses("local", &on_dal_button_press);
    DAL::subscribe_audio_frames  ("local", &on_dal_audio_frame);

    s_last_runtime = persistence::load_last_runtime_mode();
    DAL::set_driver_enabled("ir-pixmob", persistence::load_ir_enabled());
    dal::local_driver_instance()->set_pulse_enabled(
        persistence::load_screen_pulse_enabled());
    s_calibration  = persistence::load_calibration();
    s_active_mode  = nullptr;          // force enter() in enter_mode()
    enter_mode(ModeId::Boot);
}

void ModeMachine::loop_tick() {
    if (s_active_mode) s_active_mode->loop_tick();
}

void ModeMachine::switch_to(ModeId target) {
    enter_mode(target);
}

ModeId ModeMachine::current() {
    return s_active_mode ? s_active_mode->id() : ModeId::Boot;
}

const char* ModeMachine::current_name() {
    return s_active_mode ? s_active_mode->name() : "Boot";
}

}  // namespace modes
}  // namespace nocturnation
