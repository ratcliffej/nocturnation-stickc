// CONFIG_WRITE sender - see config_tx.h.

#include "ble/config_tx.h"

#include <cstring>

#include "crypto/hmac_sha256.h"
#include "transport/espnow/frame.h"

#ifdef ARDUINO
#include <Arduino.h>
#include "hal/hal.h"
#include "modes/persistence.h"
#endif

namespace nocturnation {
namespace ble {

size_t build_signed_config_write(uint8_t* out_buf, size_t out_cap,
                                 uint16_t self_source_id,
                                 uint32_t target_uid,
                                 const uint8_t target_secret[16],
                                 const uint8_t* bag_tlv, uint8_t bag_len,
                                 uint64_t numonce) {
    using namespace nocturnation::transport::espnow;

    if (!out_buf || out_cap == 0) return 0;
    if (bag_len > kConfigWriteMaxBagLen) return 0;

    ConfigWritePayload p = {};
    p.target_uid = target_uid;
    for (size_t i = 0; i < 8; ++i) {
        p.numonce[i] = static_cast<uint8_t>((numonce >> (i * 8)) & 0xFF);
    }
    p.bag_len = bag_len;
    if (bag_len > 0 && bag_tlv) {
        std::memcpy(p.bag_tlv, bag_tlv, bag_len);
    }
    // HMAC region placeholder - overwritten after signing below.
    std::memset(p.hmac, 0, kConfigWriteHmacLen);

    Header hdr = {};
    hdr.source_id       = self_source_id;
    hdr.sequence_number = 1;   // config frames don't dedup against show seq
    hdr.hop_count       = 0;

    const size_t total = encode_config_write(out_buf, out_cap, hdr, p);
    if (total == 0) return 0;

    // Sign over [envelope_header .. end_of_bag_tlv]. That's the whole
    // encoded frame minus the trailing 8-byte HMAC placeholder.
    const size_t signed_len = total - kConfigWriteHmacLen;
    uint8_t mac[kConfigWriteHmacLen];
    nocturnation::crypto::hmac_sha256_sign_t8(
        target_secret, out_buf, signed_len, mac);
    std::memcpy(out_buf + signed_len, mac, kConfigWriteHmacLen);
    return total;
}

#ifdef ARDUINO

uint64_t send_config_write(uint32_t target_uid,
                           const uint8_t target_secret[16],
                           const uint8_t* bag_tlv, uint8_t bag_len) {
    using namespace nocturnation::transport::espnow;

    const uint32_t self_uid = modes::persistence::load_device_uid();
    const uint16_t self_source_id =
        static_cast<uint16_t>(self_uid & 0xFFFFu);
    const uint64_t numonce = modes::persistence::next_config_numonce();

    uint8_t buf[kHeaderSize + kConfigWriteMaxPayloadLen];
    const size_t n = build_signed_config_write(
        buf, sizeof(buf),
        self_source_id, target_uid, target_secret,
        bag_tlv, bag_len, numonce);
    if (n == 0) return 0;

    auto* radio = hal::HAL::esp_now();
    if (!radio) {
        Serial.println("[config_tx] FAILED: hal::HAL::esp_now() returned null");
        return 0;
    }
    if (!radio->send_broadcast(buf, n)) {
        // Most common cause: ESP-NOW hasn't been initialised in the
        // current mode (Config mode doesn't by default; the caller
        // must start_broadcast() first).
        Serial.println("[config_tx] FAILED: send_broadcast returned false "
                       "(is ESP-NOW initialised?)");
        return 0;
    }

    Serial.printf("[config_tx] sent to uid=%08X numonce=%lu bag_len=%u frame=%u\n",
                  (unsigned)target_uid, (unsigned long)numonce,
                  (unsigned)bag_len, (unsigned)n);
    return numonce;
}

// -------------------------------------------------------------------------
// Epic 21 B6b: CONFIG_ACK listener ring.
// -------------------------------------------------------------------------
//
// Small ring buffer of recently-received ACKs. The ESP-NOW RX dispatch
// (installed by Config Lumes' Paired-fleet route) calls
// on_config_ack_received() when it decodes a CONFIG_ACK frame.
// wait_for_ack() scans the ring with a short polling delay.
//
// Not thread-safe in the strict sense - the ESP-NOW RX callback runs
// on the WiFi task and wait_for_ack() runs on the Arduino loop task.
// The ring uses interlocked index updates (volatile head_ += 1) and
// each slot is written in a single pass before head_ advances, so a
// torn read is impossible - either the slot has the new ACK or the
// prior one. Good enough for this use case; the alternative (a
// mutex) would need FreeRTOS primitives for a ~16-byte struct which
// is overkill.

namespace {
constexpr size_t kAckRingSize = 8;
struct AckSlot {
    bool          valid;
    ConfigAckInfo info;
    uint32_t      received_ms;
};
AckSlot          s_ack_ring[kAckRingSize] = {};
volatile size_t  s_ack_head = 0;   // next write slot (circular)
}  // namespace

void on_config_ack_received(const ConfigAckInfo& ack) {
    const size_t slot = s_ack_head % kAckRingSize;
    s_ack_ring[slot].info        = ack;
    s_ack_ring[slot].received_ms = millis();
    s_ack_ring[slot].valid       = true;
    s_ack_head = (s_ack_head + 1) % kAckRingSize;
}

void clear_ack_ring() {
    for (size_t i = 0; i < kAckRingSize; ++i) s_ack_ring[i] = {};
    s_ack_head = 0;
}

bool wait_for_ack(uint32_t uid, uint64_t numonce, uint32_t timeout_ms,
                  ConfigAckInfo& out) {
    const uint32_t start = millis();
    while ((millis() - start) < timeout_ms) {
        for (size_t i = 0; i < kAckRingSize; ++i) {
            if (!s_ack_ring[i].valid) continue;
            if (s_ack_ring[i].info.responder_uid != uid) continue;
            if (s_ack_ring[i].info.responder_numonce != numonce) continue;
            out = s_ack_ring[i].info;
            // Consume so a repeat retry doesn't re-match.
            s_ack_ring[i].valid = false;
            return true;
        }
        delay(10);
    }
    return false;
}

#endif   // ARDUINO

}  // namespace ble
}  // namespace nocturnation
