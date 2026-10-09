// CONFIG_WRITE sender (Epic 21 B4).
//
// Produces authenticated CONFIG_WRITE frames that config_rx (B3d) on
// the receiving Lume accepts. Inverse of the receiver: build an
// unsigned frame via encode_config_write, run HMAC-SHA256 over the
// signed region, overwrite the trailing 8 bytes with the HMAC.
//
// Split into two layers:
//
//   - build_signed_config_write(): pure logic, native-testable.
//     Takes everything via parameters (self_source_id, target_uid,
//     target_secret, bag_tlv, bag_len, numonce) and writes bytes
//     into a caller-owned buffer. No side effects. Native tests
//     round-trip against config_rx::process_config_write.
//
//   - send_config_write(): Arduino-only wrapper. Loads the Director's
//     source_id from its own UID, draws a fresh numonce from NVS
//     (persistence::next_config_numonce), builds the frame, and
//     broadcasts via hal::HAL::esp_now()->send_broadcast(). Returns
//     the numonce used so the caller can later correlate CONFIG_ACK.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace ble {

// Pure. Writes a signed CONFIG_WRITE frame into out_buf. Returns the
// total frame length (header + payload), or 0 on error (bag too big,
// buffer too small, or encode failure). target_secret must be 16 bytes.
size_t build_signed_config_write(uint8_t* out_buf, size_t out_cap,
                                 uint16_t self_source_id,
                                 uint32_t target_uid,
                                 const uint8_t target_secret[16],
                                 const uint8_t* bag_tlv, uint8_t bag_len,
                                 uint64_t numonce);

#ifdef ARDUINO
// Arduino wrapper. Builds + sends a signed CONFIG_WRITE to the
// addressed Lume. Draws a fresh numonce from NVS and returns it so
// the caller can correlate the eventual CONFIG_ACK. Returns 0 on
// any failure (encode, send, or NVS). target_secret must be 16 bytes.
uint64_t send_config_write(uint32_t target_uid,
                           const uint8_t target_secret[16],
                           const uint8_t* bag_tlv, uint8_t bag_len);

// CONFIG_ACK listener ring (Epic 21 B6b). The caller installs an ESP-NOW
// recv callback that dispatches CONFIG_ACK frames into this ring via
// on_config_ack_received(); subsequently wait_for_ack correlates by
// (uid, numonce) with a short timeout. Fire-and-forget semantics
// upstream — a missing ACK is "no confirmation", not a failure
// (CONFIG_WRITE paths can land without a return path for the ACK;
// see Epic 21 §Design).
struct ConfigAckInfo {
    uint32_t responder_uid;
    uint64_t responder_numonce;
    uint8_t  status;
    uint8_t  applied_keys;
};
void on_config_ack_received(const ConfigAckInfo& ack);

// Block up to `timeout_ms` for an ACK matching `uid` + `numonce`.
// Returns true on hit (fills `out`). Returns false on timeout.
bool wait_for_ack(uint32_t uid, uint64_t numonce, uint32_t timeout_ms,
                  ConfigAckInfo& out);

// Reset the ring (used when the Paired-fleet route enters, so stale
// acks from a prior session don't false-positive a new wait).
void clear_ack_ring();
#endif

}  // namespace ble
}  // namespace nocturnation
