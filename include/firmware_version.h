// NocturNation firmware version string.
//
// Single source of truth for the version string surfaced in the UI
// (boot splash bottom-right, Config > System > Firmware Version). Bumped
// at Epic close-out: each closed Epic is treated as a feature drop and
// the minor digit ticks up. Build-system overrides via a compile-time
// FIRMWARE_VERSION define still win - useful for one-off builds tagged
// against a specific commit.
//
// Block 14 close-out (Epic 4.6) bumped the canonical default to v0.5;
// previously the splash showed "v0.4-epic46" and the config screen
// independently claimed "1.0.0", which was confusing.

#pragma once

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "v0.5"
#endif

namespace nocturnation {

inline constexpr const char* kFirmwareVersion = FIRMWARE_VERSION;

}  // namespace nocturnation
