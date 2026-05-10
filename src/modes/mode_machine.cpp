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

void on_dal_input_action(const char*, const hal::InputEvent& ev) {
    if (s_active_mode) s_active_mode->on_input_action(ev);
}

}  // namespace

// Spectrum-frame routing (Epic 4.6 Block 11). Symmetric to the audio
// callback in the anonymous namespace above, but exposed at namespace
// scope so AutonomousMasterMode can pass it to DAL::subscribe_spectrum_frames
// at vis-activation time. ModeMachine does NOT subscribe to spectrum
// frames globally at begin() - that would pin
// DAL::has_spectrum_frame_subscribers() to true and defeat Block 7's
// pipeline gate. AutonomousMasterMode subscribes/unsubscribes on vis
// activation based on the active vis's PowerProfile.needs_spectrum_frame
// flag, so the gate flips live with the picker.
void on_dal_spectrum_frame(const char*, const SpectrumFrameEvent& ev) {
    if (s_active_mode) s_active_mode->on_spectrum_frame(ev);
}

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
    DAL::subscribe_input_actions ("local", &on_dal_input_action);

    s_last_runtime = persistence::load_last_runtime_mode();
    DAL::set_driver_enabled("ir-pixmob", persistence::load_ir_enabled());
    dal::local_driver_instance()->set_pulse_enabled(
        persistence::load_screen_pulse_enabled());
    s_calibration  = persistence::load_calibration();

    // One-shot migration of legacy NVS keys to their post-Block-9 homes.
    // Must run BEFORE the first enter_mode() so any later transition
    // into SlaveMode finds PixMobIrBinding's "group" property already
    // populated from the legacy slv_ir_grp key.
    persistence::migrate_legacy_nvs_keys();

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

#ifndef ARDUINO
// Native test seam. The Block 10 overlay tests need to reach the
// AutonomousMasterMode instance to read its internal state via the
// test_seam accessors on the class; the per-mode singletons live in
// the anonymous namespace above, so we expose a typed accessor
// inside this TU. Test TUs reach it via the free-function bridge
// defined just below in the global namespace.
AutonomousMasterMode& test_seam_get_autonomous_master() {
    return s_autonomous_master;
}
#endif

}  // namespace modes
}  // namespace nocturnation

#ifndef ARDUINO
// Free-function bridge so test TUs can pick up the accessor with a plain
// extern declaration; mirrors the extern "C" millis() seam at the top
// of this file. Kept thin: just forwards to the in-namespace symbol.
nocturnation::modes::AutonomousMasterMode& test_get_autonomous_master() {
    return nocturnation::modes::test_seam_get_autonomous_master();
}
#endif
