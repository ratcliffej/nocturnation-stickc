// Shared NVS persistence helpers used by ModeMachine + multiple modes.
//
// Slave-only NVS helpers (slv_ir_grp, slv_chan, slv_repeat) are NOT here;
// they live in slave_mode.cpp's anonymous namespace because no other mode
// reads or writes them.

#pragma once

#include <cstdint>

#include "modes/mode_machine.h"   // for ModeId (in include/)

namespace nocturnation {
namespace modes {
namespace persistence {

// Audio-Live calibration. Per-band log2 floor/ceiling for the spectrum
// bars, plus an auto-calibrate flag. Defaults are tuned from
// Jason's StickC Plus2 + Vengaboys reference; sound-check overrides them with
// per-device values, and auto mode bypasses these in favour of rolling
// min/max during AudioLive use (room/audience-adaptive).
struct AudioCalibration {
    float floor[4];        // B, M, T, R log2 floor
    float ceil [4];        // B, M, T, R log2 ceiling
    bool  auto_enabled;
};

constexpr AudioCalibration kCalibrationDefault = {
    /*floor=*/ { 14.0f, 14.0f, 15.0f,  5.0f },
    /*ceil =*/ { 19.0f, 20.0f, 21.0f, 10.0f },
    /*auto =*/ false,
};

constexpr ModeId kDefaultRuntimeMode = ModeId::AutonomousMaster;

bool is_persisted_runtime_mode(ModeId m);

ModeId           load_last_runtime_mode();
void             save_last_runtime_mode(ModeId m);

bool             load_ir_enabled();
void             save_ir_enabled(bool e);

bool             load_screen_pulse_enabled();
void             save_screen_pulse_enabled(bool e);

uint8_t          load_master_channel();
void             save_master_channel(uint8_t c);

AudioCalibration load_calibration();
void             save_calibration(const AudioCalibration& c);

// Accessor over the s_calibration static that lives in mode_machine.cpp
// (loaded by ModeMachine::begin(), updated by TestMode's Calibrate flow).
// Returns a reference so TestMode can read AND mutate the same instance.
AudioCalibration& current_calibration();

// Accessor over the s_last_runtime static that lives in mode_machine.cpp
// (loaded by ModeMachine::begin(), updated by enter_mode() when a
// persisted runtime mode is entered). Read by Boot (for the countdown
// label + the "switch to last runtime" target), Menu (default cursor)
// and Config (System submenu read-out).
ModeId current_last_runtime();

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
