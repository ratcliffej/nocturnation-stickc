// Property-bag TLV codec implementation (Epic 20 B3b).
//
// See property_bag_tlv.h for the wire format. This TU is host-agnostic —
// no Arduino / NimBLE / firmware headers pulled in, so native tests link
// the same object the firmware ships.

#include "property_bag_tlv.h"

namespace nocturnation {
namespace ble {

// -----------------------------------------------------------------------------
// Encoder
// -----------------------------------------------------------------------------

TlvEncoder::TlvEncoder(uint8_t* buf, size_t buflen)
    : buf_(buf), buflen_(buflen), len_(0), count_(0) {
    // Reserve byte 0 for entry_count. Advance len_ past it so subsequent
    // writes append rather than overwrite the header. If the buffer is
    // zero-length, we hold at len_=0 and every add_* call fails.
    if (buflen_ >= 1) {
        buf_[0] = 0;
        len_    = 1;
    }
}

bool TlvEncoder::write_entry(const char* key,
                             ValueType type,
                             const uint8_t* value,
                             uint8_t value_len) {
    if (!buf_ || buflen_ == 0 || !key) return false;
    if (count_ == 0xFF) return false;   // entry_count would overflow

    // Compute key length. Bail if it doesn't fit in a u8 or is empty.
    size_t key_len_full = 0;
    while (key[key_len_full] != '\0') {
        if (key_len_full == 0xFF) return false;   // key_len must fit in u8
        ++key_len_full;
    }
    if (key_len_full == 0) return false;   // empty keys are always malformed
    const uint8_t key_len = static_cast<uint8_t>(key_len_full);

    // Space needed: key_len (1) + key + value_type (1) + value_len (1) + value
    const size_t need = 1u + key_len + 1u + 1u + value_len;
    if (len_ + need > buflen_) return false;

    uint8_t* p = buf_ + len_;
    *p++ = key_len;
    for (uint8_t i = 0; i < key_len; ++i) *p++ = static_cast<uint8_t>(key[i]);
    *p++ = static_cast<uint8_t>(type);
    *p++ = value_len;
    if (value_len > 0 && value != nullptr) {
        for (uint8_t i = 0; i < value_len; ++i) *p++ = value[i];
    }

    len_ += need;
    ++count_;
    buf_[0] = count_;
    return true;
}

bool TlvEncoder::add_u8(const char* key, uint8_t value) {
    const uint8_t v = value;
    return write_entry(key, ValueType::U8, &v, 1);
}

bool TlvEncoder::add_u16(const char* key, uint16_t value) {
    const uint8_t v[2] = {
        static_cast<uint8_t>(value & 0xFF),
        static_cast<uint8_t>((value >> 8) & 0xFF),
    };
    return write_entry(key, ValueType::U16, v, 2);
}

bool TlvEncoder::add_u32(const char* key, uint32_t value) {
    const uint8_t v[4] = {
        static_cast<uint8_t>( value        & 0xFF),
        static_cast<uint8_t>((value >>  8) & 0xFF),
        static_cast<uint8_t>((value >> 16) & 0xFF),
        static_cast<uint8_t>((value >> 24) & 0xFF),
    };
    return write_entry(key, ValueType::U32, v, 4);
}

bool TlvEncoder::add_bool(const char* key, bool value) {
    const uint8_t v = value ? 0x01 : 0x00;
    return write_entry(key, ValueType::Bool, &v, 1);
}

bool TlvEncoder::add_bytes(const char* key, const uint8_t* value, uint8_t value_len) {
    return write_entry(key, ValueType::Bytes, value, value_len);
}

bool TlvEncoder::add_utf8(const char* key, const char* value) {
    if (!value) return write_entry(key, ValueType::Utf8, nullptr, 0);
    size_t n = 0;
    while (value[n] != '\0') {
        if (n == 0xFF) return false;
        ++n;
    }
    return write_entry(key, ValueType::Utf8,
                       reinterpret_cast<const uint8_t*>(value),
                       static_cast<uint8_t>(n));
}

bool TlvEncoder::add_utf8(const char* key, const char* value, uint8_t value_len) {
    return write_entry(key, ValueType::Utf8,
                       reinterpret_cast<const uint8_t*>(value), value_len);
}

// -----------------------------------------------------------------------------
// Decoder
// -----------------------------------------------------------------------------

DecodeError decode_property_bag(const uint8_t* buf, size_t buflen,
                                DecodeCallback cb, void* ctx) {
    if (!buf || buflen == 0) return DecodeError::EmptyBuffer;
    if (!cb) return DecodeError::Malformed;

    const uint8_t entry_count = buf[0];
    size_t off = 1;

    for (uint8_t i = 0; i < entry_count; ++i) {
        // Every entry has: key_len (1) + key + value_type (1) + value_len (1) + value.
        if (off + 1 > buflen) return DecodeError::Malformed;
        const uint8_t key_len = buf[off++];
        if (key_len == 0) return DecodeError::Malformed;
        if (off + key_len > buflen) return DecodeError::Malformed;
        const char* key = reinterpret_cast<const char*>(buf + off);
        off += key_len;

        if (off + 1 > buflen) return DecodeError::Malformed;
        const uint8_t type_byte = buf[off++];

        if (off + 1 > buflen) return DecodeError::Malformed;
        const uint8_t value_len = buf[off++];
        if (off + value_len > buflen) return DecodeError::Malformed;
        const uint8_t* value = (value_len > 0) ? (buf + off) : nullptr;
        off += value_len;

        TlvEntry entry{
            key,
            key_len,
            static_cast<ValueType>(type_byte),
            value,
            value_len,
        };
        if (!cb(entry, ctx)) return DecodeError::CallbackAbort;
    }

    return DecodeError::Ok;
}

}  // namespace ble
}  // namespace nocturnation
