// Shared NVS persistence helpers used by ModeMachine + multiple modes.
//
// Slave-only NVS keys (slv_chan, slv_repeat) are owned here as of
// Epic 4.6 Block 9 because both SlaveMode and ConfigMode read/write them
// and the per-mode anonymous-namespace copies were drifting. The third
// slave key (slv_ir_grp) is gone - migrated to PixMobIrBinding's
// property bag (NVS namespace "nb_pixmob-ir", key "group") via
// migrate_legacy_nvs_keys() on first boot after the upgrade.

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

// Slave ESP-NOW channel preference. 0 = auto (dual-channel scan with
// show priority), 1 / 6 / 11 = locked to that channel. Defaults to 0.
// SlaveMode reads on enter(); ConfigMode mutates from the operator menu.
uint8_t          load_slave_channel();
void             save_slave_channel(uint8_t c);

// Slave repeater mode. When enabled, slave rebroadcasts each unique
// frame with hop_count + 1 (capped at spec §4.3's 3-hop limit). Off
// by default. Receive-rebroadcast is an ESP-NOW transport concern, not
// an output-binding concern, so it stays a slave-mode setting.
bool             load_slave_repeat_enabled();
void             save_slave_repeat_enabled(bool e);

// Slave NocturNation group ID (Epic 4.65 Block 5). Device-wide value
// used by SlaveMode's receive filter: a binding is local (not a relay)
// only fires when LIGHT_COMMAND.target_group is 0 OR matches this
// value. Default 0 ("respond to everything"). Range 0-255; operator
// sets via Config > Slave > Group. Distinct from PixMobIrBinding's
// own "group" property which is the PixMob protocol's IR group code
// (output concern), not the NocturNation receive-filter group.
uint8_t          load_slv_group();
void             save_slv_group(uint8_t g);

// Active master-side visualisation id. AutonomousMasterMode resolves the
// returned id against visualisation_registry() on enter(); if the saved
// id no longer resolves to a registered vis (uninstalled, renamed) the
// mode falls back to the canonical "beat-pulse" default. The returned
// pointer is into a static buffer owned by this TU and is valid until
// the next call to load_active_vis_id() - copy or strdup if you need to
// hold onto it across other persistence calls.
//
// Plugin id() is capped at 12 chars by convention (the 15-char NVS
// namespace limit minus the "nv_" prefix), so the 16-byte storage here
// has comfortable headroom.
const char*      load_active_vis_id();
void             save_active_vis_id(const char* id);

// One-shot NVS migration from pre-Block-9 keys to their new homes.
// Called from ModeMachine::begin() BEFORE enter_mode(Boot) so the
// property bags populated here are visible when SlaveMode is later
// entered. Idempotent: removes legacy keys after migrating so the
// second call is a no-op.
//
// Current migrations:
//   noct/slv_ir_grp -> nb_pixmob-ir/group  (Block 9; slave IR forward
//                                           group moved from a slave-
//                                           mode private NVS key to
//                                           PixMobIrBinding's property
//                                           bag).
void             migrate_legacy_nvs_keys();

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

#ifndef ARDUINO
// Native test seam. The slave-mode persistence helpers fall through to
// process-static storage on native; tests reach in via these helpers to
// seed legacy-key state (for migrate_legacy_nvs_keys coverage) and to
// reset the static between test cases.
namespace test_seam {
void seed_legacy_slv_ir_grp(uint8_t g);
void clear_native_persistence();
}  // namespace test_seam
#endif

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
