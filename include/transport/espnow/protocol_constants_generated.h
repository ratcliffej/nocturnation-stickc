// AUTO-GENERATED from Docs/protocol/constants.yaml. Do not edit by hand.
//
// To regenerate, run tools/regen_constants.sh from the firmware repo
// root. The accompanying check script re-runs the generator and
// fails on any drift between this header and the SOT.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace transport {
namespace espnow {

constexpr uint8_t kMagic0          = 0x4E;
constexpr uint8_t kMagic1          = 0x4E;
constexpr uint8_t kProtocolVersion = 0x02;
constexpr uint8_t kHeaderSize      = 8;

enum class MessageType : uint8_t {
    Heartbeat      = 0x00,
    LightPulse     = 0x03,
    LightWash      = 0x06,
    LightWashEnd   = 0x07,
    LightWashPulse = 0x08,
    Extension      = 0xFF,
};

// Payload bytes per message type (excluding the 8-byte header).
constexpr uint8_t kHeartbeatPayloadLen      = 9;
constexpr uint8_t kLightPulsePayloadLen     = 9;
constexpr uint8_t kLightWashPayloadLen      = 16;
constexpr uint8_t kLightWashEndPayloadLen   = 3;
constexpr uint8_t kLightWashPulsePayloadLen = 9;

}  // namespace espnow
}  // namespace transport
}  // namespace nocturnation
