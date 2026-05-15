// DeviceClass - operator-facing taxonomy of OutputBinding render targets.
//
// Lighting designers think in device classes ("light bracelets", "screen
// devices", "multi-LED+screen devices"), not capability sets. Class is
// the Director-side addressing axis paired with group ID; the
// "<class>:<group>" target string passed to render_fx parses into these
// two bytes, and Lumes filter inbound LIGHT_COMMAND on
// (target_class, target_group) against each active OutputBinding's
// (class(), configured_group). See architecture spec §4.3 + §7.6.
//
// Enum values are wire-stable: they ride the target_class byte of
// LIGHT_COMMAND payloads and persist into NVS. Reorder them at your
// peril. 0x04..0xFF are reserved for future classes (accelerometer
// stick, smoke machine, ...). A future class addition is a one-line
// enum extension - no protocol bump needed.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace hal {

enum class DeviceClass : uint8_t {
    All            = 0x00,  // addressing wildcard - never returned by a binding
    Light          = 0x01,  // light-only devices (PixMob X4 bracelets, future LED wristbands)
    Screen         = 0x02,  // screen-only devices (StickC LCD)
    MultiLedScreen = 0x03,  // multi-LED + screen devices (Tildagon, Epic 5)
    // 0x04..0xFF reserved.
};

}  // namespace hal
}  // namespace nocturnation
