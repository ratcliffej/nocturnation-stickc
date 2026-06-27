// DeviceClass - operator-facing taxonomy of OutputBinding render targets.
//
// Lighting designers think in device classes ("light bracelets", "screen
// devices", "multi-LED+screen devices"), not capability sets. Class is
// the Director-side addressing axis paired with group ID; the
// "<class>:<group>" target string passed to render_fx parses into these
// two bytes, and Lumes filter inbound LIGHT_PULSE on
// (target_class, target_group) against each active OutputBinding's
// (class(), configured_group). See architecture spec §4.3 + §7.6.
//
// Enum values are wire-stable: they ride the target_class byte of
// LIGHT_PULSE payloads and persist into NVS. Reorder them at your
// peril. 0x04..0xFF are reserved for future classes (accelerometer
// stick, smoke machine, ...). A future class addition is a one-line
// enum extension - no protocol bump needed.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace hal {

enum class DeviceClass : uint8_t {
    All            = 0x00,  // addressing wildcard - never returned by a binding
    Light          = 0x01,  // light-only devices (PixMob X4 bracelets, LED strips)
    Screen         = 0x02,  // RESERVED - legacy LocalDisplayBinding class, retired in Epic 13
    MultiLedScreen = 0x03,  // multi-LED + screen devices (Tildagon, Epic 5)
    Display        = 0x04,  // Epic 13: text + bitmap rendering surfaces (Stick LCD, Tildagon badge)
    // 0x05..0xFF reserved.
};

}  // namespace hal
}  // namespace nocturnation
