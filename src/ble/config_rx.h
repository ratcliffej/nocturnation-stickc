// CONFIG_WRITE receive-side dispatcher (Epic 21 B3d).
//
// Composes the four shared TUs from B3a-B3c:
//   - transport/espnow/frame.*   (decode CONFIG_WRITE payload)
//   - crypto/hmac_sha256.*       (verify truncated HMAC)
//   - transport/espnow/numonce_lru.* (reject replays)
//   - ble/config_apply.*         (apply recognised property-bag keys)
//
// into the receive-side entry point the Lume ESP-NOW handler calls
// when a CONFIG_WRITE frame arrives. Verification order per the
// Epic 21 design doc:
//
//   1. target_uid match                     -> BadTarget (silent)
//   2. numonce check (per-sender LRU)       -> Replay (silent)
//   3. HMAC verify over [header..bag_tlv]   -> BadHmac (silent)
//   4. decode property bag                  -> MalformedBag (silent)
//   5. walk bag + apply recognised keys     -> Accepted
//
// Silent drops at every rejection path deny a timing oracle on the
// HMAC verification step and give an attacker no feedback distinguishing
// "wrong target" from "wrong signature". The caller only emits CONFIG_ACK
// + ack-flash on Accepted.
//
// Native-safe: no Arduino, no NimBLE, no ESP-IDF. Takes everything
// via parameters; the Arduino wrapper (B3e / LumeMode) binds the real
// NumonceLru, secret, and apply callback. Tests mock the apply_fn to
// capture entries without touching NVS.

#pragma once

#include <cstddef>
#include <cstdint>

#include "ble/property_bag_tlv.h"
#include "transport/espnow/numonce_lru.h"

namespace nocturnation {
namespace ble {

enum class ConfigRxResult : uint8_t {
    Accepted       = 0,  // write applied; caller should emit CONFIG_ACK with status
    BadTarget      = 1,  // target_uid != self_uid; silent drop
    Replay         = 2,  // numonce not strictly greater than last-seen; silent drop
    BadHmac        = 3,  // HMAC mismatch; silent drop (no oracle)
    MalformedFrame = 4,  // decode_config_write failed
    MalformedBag   = 5,  // decode_property_bag failed
};

struct ConfigRxDetail {
    ConfigRxResult result;
    uint64_t       numonce;              // sender's numonce; echoed in CONFIG_ACK on Accepted
    uint8_t        applied_keys;         // bag entries that walked (recognised + unrecognised)
    uint8_t        config_write_status;  // 0x00 clean, 0x82 any value-out-of-range on recognised keys
};

// Process one incoming CONFIG_WRITE frame.
//
// `frame_bytes` points to the raw received frame (envelope header +
// payload). `frame_len` is its total length as the ESP-NOW layer
// delivered it. `self_uid` and `self_secret` come from persistence.
// `numonces` is the Lume's single shared NumonceLru instance.
// `apply_fn` + `apply_ctx` are passed verbatim to decode_property_bag
// so recognised keys land in NVS (or in a test mock).
//
// Returns the result + diagnostic detail. On Accepted the numonce LRU
// has been updated and the apply callback has walked the bag.
ConfigRxDetail process_config_write(
    const uint8_t* frame_bytes,
    size_t         frame_len,
    uint32_t       self_uid,
    const uint8_t  self_secret[16],
    nocturnation::transport::espnow::NumonceLru& numonces,
    nocturnation::ble::DecodeCallback apply_fn,
    void* apply_ctx);

}  // namespace ble
}  // namespace nocturnation
