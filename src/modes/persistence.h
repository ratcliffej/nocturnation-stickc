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

constexpr ModeId kDefaultRuntimeMode = ModeId::Lume;

bool is_persisted_runtime_mode(ModeId m);

ModeId           load_last_runtime_mode();
void             save_last_runtime_mode(ModeId m);

bool             load_ir_enabled();
void             save_ir_enabled(bool e);

// IR emitter selection. The master ir_en flag above turns the whole PixMob
// IR path on/off; these two select which physical emitter(s) the enabled
// driver fans the encoded frame out to. Internal is the built-in LED
// (default ON - the behaviour every device had before the external option
// existed); external is an M5Stack IR unit on the Plus2 GPIO 26 header
// (default OFF - not assumed present on a fresh device).
bool             load_internal_ir_enabled();
void             save_internal_ir_enabled(bool e);
bool             load_external_ir_enabled();
void             save_external_ir_enabled(bool e);

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
// receive filter: a non-relay binding fires when LIGHT_PULSE
// target_group is 0 (the broadcast group; everyone responds) OR
// matches this value exactly. A device with slv_group == 0 has no
// group and only responds to broadcasts; it does not act as a
// receive-wildcard. Range 0-255; operator sets via Config > Group.
//
// First-boot default is a random value in {1, 2, 3} assigned inline
// by migrate_legacy_nvs_keys() on first boot and persisted under the
// slv_group NVS key (retained for backwards compatibility).
// This puts fresh devices into one of the three "drum" groups that
// DynamicShow routes kick / snare / hi-hat to, so a small fleet of
// freshly-flashed Sticks naturally distributes across per-drum
// addressing without the operator touching anything.
uint8_t          load_lume_group();
void             save_lume_group(uint8_t g);

// LED strip brightness cap (Epic 12 B5 follow-on). Uniform 0..100 %
// multiplier applied to the wash + pulse render path inside
// LedStripDriver. Defaults to 25 % - SK6812 at full white draws
// ~60 mA/pixel, so 30 pixels at 100 % can exceed the Grove 5 V rail's
// 500 mA budget on Atom Lite. 25 % gives a comfortable margin while
// still being clearly visible. Operator can step up via Btn1 in
// Lume mode (cycles 100 / 50 / 25 / 10).
//
// Brightness does NOT scale LumeMode's signal-state overlay (the
// green pilot on pixel 0): that's system UI and stays at fixed
// brightness regardless of device brightness.
uint8_t          load_strip_brightness();
void             save_strip_brightness(uint8_t pct);

// Per-strip render gate (Config > LED Strip > Enable). When false,
// LumeMode disables the led-strip driver so no pixel writes hit the
// hardware. Persisted as `strip_en` (uchar 0/1).
bool             load_strip_enabled();
void             save_strip_enabled(bool e);

// Physical chain length the operator has plugged in (Config > LED
// Strip > Chain Size). Drives HAL::LedStrip::set_pixel_count() so a
// chained 144-pixel run actually walks all pixels (the HAL backend
// allocates buffer space at construction-time max and updateLength
// resizes at runtime). Persisted as `strip_cnt` (ushort).
uint16_t         load_strip_chain_size();
void             save_strip_chain_size(uint16_t n);

// Visual group size (Config > LED Strip > Group Size). Pixels within
// a group share one CHANCE roll per pulse. Default 12 matches the
// Tildagon perimeter ring's chunkiness. Persisted as `strip_grp` (uchar).
uint8_t          load_strip_group_size();
void             save_strip_group_size(uint8_t n);

// -------------------------------------------------------------------------
// Compile-time strip defaults (Epic 12 follow-on)
// -------------------------------------------------------------------------
//
// First-boot fallback values for each strip-related NVS key. Per-env
// overrides land via platformio.ini's build_flags, e.g. an Atom Lite
// deployed with a 1 m strip baked in:
//
//   [env:m5stack-atomlite]
//   build_flags =
//     ${env:firmware-base.build_flags}
//     -DNOCT_DEFAULT_STRIP_CHAIN_SIZE=144
//
// On hosts without a Config menu (Atom Lite) the build-time defaults
// are the only way to configure the strip; on Sticks the operator
// can still override at runtime via Config > LED Strip.
//
// To make a reflash *authoritative* - replacing whatever NVS already
// holds with the compile-time values - add `-DNOCT_STRIP_FORCE_DEFAULTS=1`
// to the env. apply_strip_force_defaults() (called at boot) compares
// the build tag (__DATE__ + __TIME__) against the last-applied tag
// in NVS and writes the defaults exactly once per fresh build.

#ifndef NOCT_DEFAULT_STRIP_ENABLED
#define NOCT_DEFAULT_STRIP_ENABLED 1
#endif
#ifndef NOCT_DEFAULT_STRIP_BRIGHTNESS
// First-install default: 1 %. Picked to be brownout-safe on every
// power source we ship - battery, USB-CDC laptop, wall charger -
// regardless of how many pixels the strip has, what colour is
// requested, or what mode the operator boots into. A fresh device
// runs visibly dim until the operator opens Config > LED Strip and
// dials it up to a level their power supply can sustain. This is
// the deliberately-conservative end of the {50, 25, 10, 1} cycle
// (changed 2026-06-24 after bench-confirmed brownout reports on
// the Pulse / Whiteout / raw-RGB-white paths at higher defaults).
#define NOCT_DEFAULT_STRIP_BRIGHTNESS 1
#endif
#ifndef NOCT_DEFAULT_STRIP_GROUP_SIZE
#define NOCT_DEFAULT_STRIP_GROUP_SIZE 12
#endif
#ifndef NOCT_DEFAULT_STRIP_CHAIN_SIZE
#define NOCT_DEFAULT_STRIP_CHAIN_SIZE 29
#endif

constexpr bool     kDefaultStripEnabled    = (NOCT_DEFAULT_STRIP_ENABLED != 0);
constexpr uint8_t  kDefaultStripBrightness = NOCT_DEFAULT_STRIP_BRIGHTNESS;
constexpr uint8_t  kDefaultStripGroupSize  = NOCT_DEFAULT_STRIP_GROUP_SIZE;
constexpr uint16_t kDefaultStripChainSize  = NOCT_DEFAULT_STRIP_CHAIN_SIZE;

#ifdef NOCT_STRIP_FORCE_DEFAULTS
constexpr bool kStripForceDefaults = (NOCT_STRIP_FORCE_DEFAULTS != 0);
#else
constexpr bool kStripForceDefaults = false;
#endif

// Apply the compile-time strip defaults to NVS on the first boot of
// each fresh build, IF NOCT_STRIP_FORCE_DEFAULTS is set. Uses
// __DATE__ + __TIME__ as the per-build tag so every reflash triggers
// exactly one override. No-op when the flag isn't set, or when the
// stored tag already matches this build's tag. Called once from
// ModeMachine::begin() before any load_strip_* consumer reads NVS.
void apply_strip_force_defaults();

// Director source_id for channel 1 (community range, 0x00-0x3F) per
// protocol manual §3.4. Stable per device: chosen randomly within the
// community range on first boot by migrate_legacy_nvs_keys, persisted
// to NVS key "mst_src_id" under the "noct" namespace, and reused on
// every subsequent boot. A returning Lume therefore recognises the
// same Director across power-cycles, which is the contract channel 1
// is built on.
//
// The value is intentionally pre-rolled on every device regardless of
// configured channel: if the operator later moves the device to a
// Director-on-channel-1 deployment the ID is ready, and the dormant
// key on a Lume-only or channel-11 device costs one byte of NVS. On
// channel 11 the Director allocates a fresh Performance-range ID at
// every boot via a different path (Epic 5.5 B4); this key is not
// consulted there.
//
// Validation: a value persisted outside the community range (e.g. by
// older firmware, NVS corruption) is treated as missing - the next
// migrate_legacy_nvs_keys call re-rolls. Production code calling
// load_director_source_id() can rely on the return value being in
// [0x00, 0x3F] as long as migrate has run.
uint8_t          load_director_source_id();
void             save_director_source_id(uint8_t id);

// Director Performance-range source id (channel 11). Like its
// community-range cousin above, but in 0x40..0xFE. Random on first
// install, persisted to NVS, sticky across boots and listen-before-
// broadcast re-rolls. Operator can re-roll via the Config menu.
//
// Why sticky (changed 2026-06-24): the original Epic 5.5 B4 design
// was random-per-boot on the theory that each show is its own
// session. EMF multi-show partitioning needs a STABLE identity per
// Director - so a Lume that locks to "Director A" actually stays
// locked across A's restarts, and the orchestrator's display-family
// frames can be tagged with the Director's id (rather than the
// 0xFF broadcast slot) and only render on A's locked Lumes.
//
// load returns the persisted value if present in [0x40, 0xFE]; if
// the key is absent OR the value falls outside the Performance
// range (older firmware / corruption) it rolls a fresh random one,
// persists, and returns it.
uint8_t          load_director_perf_source_id();
void             save_director_perf_source_id(uint8_t id);

// Roll a fresh random Performance-range source id, persist it, and
// return it. Used by the Config UI's "DirectorID > Re-roll" action
// and by the listen-before-broadcast collision-recovery path. The
// rolled value is in [0x40, 0xFE]; calling repeatedly produces
// independent rolls. Native build returns a deterministic value
// driven by a hook (see persistence.cpp's native section) so tests
// can pin the outcome.
uint8_t          reroll_director_perf_source_id();

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
// Set the deterministic stand-in for esp_random() & 0x3F used by
// the native migrate_legacy_nvs_keys() first-boot mst_src_id path.
// Must be in [0x00, 0x3F] (the community range).
void set_first_boot_director_src_id_rng(uint8_t id_in_community_range);
// Plant a raw mst_src_id value in the native NVS stand-in, bypassing
// the save_director_source_id clamp. Used to simulate the
// "corrupted NVS / older firmware wrote an out-of-range value"
// scenario so the migrate re-roll path can be exercised on native.
void plant_raw_director_src_id(uint8_t id);
// Set the deterministic stand-in for esp_random() % 191 + 0x40 used
// by load_director_perf_source_id() on the first-install path
// (key absent / out of range). Must be in [0x40, 0xFE].
void set_first_boot_director_perf_src_id_rng(uint8_t id_in_perf_range);
// Set the deterministic stand-in for esp_random() % 191 + 0x40 used
// by reroll_director_perf_source_id() (Config UI re-roll, listen-
// collision re-roll). Must be in [0x40, 0xFE].
void set_reroll_director_perf_src_id_rng(uint8_t id_in_perf_range);
// Plant a raw mst_pid value in the native NVS stand-in, bypassing
// the save_director_perf_source_id clamp. Used to simulate "old
// firmware wrote out-of-range" so the first-install re-roll path
// can be exercised.
void plant_raw_director_perf_src_id(uint8_t id);
void clear_native_persistence();
}  // namespace test_seam
#endif

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
