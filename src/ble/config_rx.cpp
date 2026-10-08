// CONFIG_WRITE dispatcher - see config_rx.h.

#include "ble/config_rx.h"

#include <cstring>

#include "crypto/hmac_sha256.h"
#include "transport/espnow/frame.h"

namespace nocturnation {
namespace ble {

namespace {

// Read a little-endian u64 from 8 bytes.
inline uint64_t read_u64_le(const uint8_t* src) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | src[i];
    return v;
}

}  // namespace

ConfigRxDetail process_config_write(
    const uint8_t* frame_bytes,
    size_t         frame_len,
    uint32_t       self_uid,
    const uint8_t  self_secret[16],
    nocturnation::transport::espnow::NumonceLru& numonces,
    DecodeCallback apply_fn,
    void*          apply_ctx)
{
    using nocturnation::transport::espnow::DecodeResult;
    using nocturnation::transport::espnow::Header;
    using nocturnation::transport::espnow::ConfigWritePayload;
    using nocturnation::transport::espnow::kHeaderSize;
    using nocturnation::transport::espnow::kConfigWriteHmacLen;
    using nocturnation::transport::espnow::decode_header;
    using nocturnation::transport::espnow::decode_config_write;

    ConfigRxDetail detail = {};
    detail.result              = ConfigRxResult::MalformedFrame;
    detail.numonce             = 0;
    detail.applied_keys        = 0;
    detail.config_write_status = 0x00;

    Header hdr = {};
    if (decode_header(frame_bytes, frame_len, hdr) != DecodeResult::Ok) {
        return detail;
    }
    // The caller usually already dispatched on hdr.message_type to
    // reach us, but double-check so a mis-wired caller fails cleanly.
    if (hdr.message_type != nocturnation::transport::espnow::MessageType::ConfigWrite) {
        return detail;
    }

    ConfigWritePayload cw = {};
    if (decode_config_write(hdr, frame_bytes + kHeaderSize, hdr.payload_len, cw)
            != DecodeResult::Ok) {
        return detail;
    }

    // 1. Target UID match.
    if (cw.target_uid != self_uid) {
        detail.result = ConfigRxResult::BadTarget;
        return detail;
    }

    const uint64_t incoming_numonce = read_u64_le(cw.numonce);

    // 2. Numonce replay check. NumonceLru keys on source_id from the
    //    ESP-NOW envelope header, NOT the target_uid - that's the
    //    sender's identity. One Lume can have multiple Director
    //    senders tracked independently.
    if (!numonces.check_and_update(hdr.source_id, incoming_numonce)) {
        detail.result = ConfigRxResult::Replay;
        return detail;
    }

    // 3. HMAC verify. The signed region is [envelope_header .. end_of_bag_tlv].
    //    That's `frame_len - kConfigWriteHmacLen` bytes - the trailing
    //    8 bytes are the HMAC itself.
    //
    //    IMPORTANT: numonces.check_and_update() has already updated
    //    the LRU above. If the HMAC fails here we've burned a numonce
    //    slot on a forged frame. That's acceptable: the only harm is
    //    that a future legitimate CONFIG_WRITE with a numonce <= the
    //    forged one would be rejected as replay, which just means the
    //    operator retries with a fresh numonce (Q7). Alternative
    //    (verify-before-numonce) would let an attacker test numonce
    //    acceptance without a valid signature - a weak oracle. Prefer
    //    the current ordering.
    if (frame_len < kConfigWriteHmacLen) {
        detail.result = ConfigRxResult::BadHmac;
        return detail;
    }
    const size_t signed_len = frame_len - kConfigWriteHmacLen;
    if (!nocturnation::crypto::hmac_sha256_verify_t8(
            self_secret, frame_bytes, signed_len, cw.hmac)) {
        detail.result = ConfigRxResult::BadHmac;
        return detail;
    }

    // 4. + 5. Decode + apply the property bag. The apply callback is
    //    the caller's choice: Arduino firmware passes apply_config_entry
    //    with an ApplyCtx bound to the device role; tests pass a mock
    //    that captures entries.
    struct WalkCountCtx {
        DecodeCallback user_fn;
        void*          user_ctx;
        uint8_t        walked;
    };
    WalkCountCtx wc = { apply_fn, apply_ctx, 0 };
    auto counting_cb = [](const TlvEntry& e, void* raw_ctx) -> bool {
        auto* w = static_cast<WalkCountCtx*>(raw_ctx);
        if (w->walked < 0xFF) ++w->walked;
        return w->user_fn ? w->user_fn(e, w->user_ctx) : true;
    };
    const DecodeError err = decode_property_bag(
        cw.bag_tlv, cw.bag_len, counting_cb, &wc);
    if (err != DecodeError::Ok) {
        detail.result = ConfigRxResult::MalformedBag;
        return detail;
    }

    detail.result        = ConfigRxResult::Accepted;
    detail.numonce       = incoming_numonce;
    detail.applied_keys  = wc.walked;
    // config_write_status: if the caller's apply ctx is an ApplyCtx
    // and it set value_out_of_range during the walk, surface 0x82.
    // We can't peek the generic void* here safely, so the caller
    // inspects their own ctx after return and sets the ack's status
    // field accordingly. Default 0x00; the Arduino wrapper overwrites.
    detail.config_write_status = 0x00;
    return detail;
}

}  // namespace ble
}  // namespace nocturnation
