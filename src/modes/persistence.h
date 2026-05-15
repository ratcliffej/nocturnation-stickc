// Shared NVS persistence helpers used by ModeMachine + multiple modes.
//
// Lume-only NVS keys (slv_chan, slv_repeat) are owned here as of
// Epic 4.6 Block 9 because both LumeMode and ConfigMode read/write them
// and the per-mode anonymous-namespace copies were drifting. The third
// Lume key (slv_ir_grp) is gone - migrated to PixMobIrBinding's
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

constexpr ModeId kDefaultRuntimeMode = ModeId::Director;

bool is_persisted_runtime_mode(ModeId m);

ModeId           load_last_runtime_mode();
void             save_last_runtime_mode(ModeId m);

bool             load_ir_enabled();
void             save_ir_enabled(bool e);

bool             load_screen_pulse_enabled();
void             save_screen_pulse_enabled(bool e);

uint8_t          load_director_channel();
void             save_director_channel(uint8_t c);

// Lume ESP-NOW channel preference. 0 = auto (dual-channel scan with
// show priority), 1 / 6 / 11 = locked to that channel. Defaults to 0.
// LumeMode reads on enter(); ConfigMode mutates from the operator menu.
uint8_t          load_lume_channel();
void             save_lume_channel(uint8_t c);

// Lume repeater mode. When enabled, Lume rebroadcasts each unique
// frame with hop_count + 1 (capped at spec §4.3's 3-hop limit). Off
// by default. Receive-rebroadcast is an ESP-NOW transport concern, not
// an output-binding concern, so it stays a Lume-mode setting.
bool             load_lume_repeat_enabled();
void             save_lume_repeat_enabled(bool e);

// Lume NocturNation group ID. Device-wide value used by LumeMode's
// receive filter: a non-relay binding fires when LIGHT_COMMAND
// target_group is 0 (the broadcast group; everyone responds) OR
// matches this value exactly. A device with slv_group == 0 has no
// group and only responds to broadcasts; it does not act as a
// receive-wildcard. Range 0-255; operator sets via Config > Group.
//
// First-boot default is a random value in {1, 2, 3} assigned by
// ensure_slv_group_assigned() and persisted on first migrate call.
// This puts fresh devices into one of the three "drum" groups that
// DynamicShow routes kick / snare / hi-hat to, so a small fleet of
// freshly-flashed Sticks naturally distributes across per-drum
// addressing without the operator touching anything.
uint8_t          load_lume_group();
void             save_lume_group(uint8_t g);

// Active Director-side visualisation id. Pre-Epic-4.7 selection key;
// retained for read-side back-compat during migration. Block 1 of
// Epic 4.7 retires this in favour of active_show; the value is
// consumed by migrate_legacy_nvs_keys on first boot post-upgrade.
// Block 2 will retire the Visualisation framework entirely.
const char*      load_active_vis_id();
void             save_active_vis_id(const char* id);

// Active Director-side Show id (Epic 4.7 Block 1). DirectorMode
// resolves the returned id against show_registry() on enter(); if the
// saved id no longer resolves to a registered Show (uninstalled,
// renamed) the mode falls back to the canonical "simple-beat" default.
// The returned pointer is into a static buffer owned by this TU and is
// valid until the next call to load_active_show_id() - copy or strdup
// if you need to hold onto it across other persistence calls.
//
// Plugin id() is capped at 12 chars by convention (the 15-char NVS
// namespace limit minus the "ns_" prefix), so the 16-byte storage here
// has comfortable headroom.
const char*      load_active_show_id();
void             save_active_show_id(const char* id);

// One-shot NVS migration from pre-Block-9 keys to their new homes.
// Called from ModeMachine::begin() BEFORE enter_mode(Boot) so the
// property bags populated here are visible when LumeMode is later
// entered. Idempotent: removes legacy keys after migrating so the
// second call is a no-op.
//
// Current migrations:
//   noct/slv_ir_grp -> nb_pixmob-ir/group  (Block 9; Lume IR forward
//                                           group moved from a Lume-
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
// Native test seam. The Lume-mode persistence helpers fall through to
// process-static storage on native; tests reach in via these helpers to
// seed legacy-key state (for migrate_legacy_nvs_keys coverage) and to
// reset the static between test cases.
namespace test_seam {
void seed_legacy_slv_ir_grp(uint8_t g);
// Seed the legacy active_vis key so migrate_legacy_nvs_keys can be
// exercised on native. Sets the in-process active_vis buffer to `id`
// and arms the migration flag; the next migrate call consumes it.
void seed_legacy_active_vis(const char* id);
// Set the deterministic stand-in for esp_random() % 3 + 1 used by
// the native migrate_legacy_nvs_keys() first-boot slv_group path.
// Must be in {1, 2, 3} to mirror what esp_random() would emit.
void set_first_boot_rng(uint8_t g_in_1_3);
void clear_native_persistence();
}  // namespace test_seam
#endif

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
