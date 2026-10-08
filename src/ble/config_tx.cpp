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
    if (!radio) return 0;
    if (!radio->send_broadcast(buf, n)) return 0;

    Serial.printf("[config_tx] sent to uid=%08X numonce=%lu bag_len=%u\n",
                  (unsigned)target_uid, (unsigned long)numonce, (unsigned)bag_len);
    return numonce;
}

#endif   // ARDUINO

}  // namespace ble
}  // namespace nocturnation
