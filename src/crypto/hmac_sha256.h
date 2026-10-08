// HMAC-SHA256 for the NocturNation Epic 21 authenticated config channel.
//
// A deliberately tiny pure-C++ translation unit, linked identically on
// Arduino and native. The reason it's not mbedtls on Arduino + stubs on
// native is that we want byte-identical results from the same source
// code on both targets - the RFC 4231 known-answer tests prove the
// implementation is correct, and the firmware uses the exact same bits.
//
// 16-byte key is NocturNation's Epic 21 secret length ([[project-
// device-uid-design]]); the API is scoped to that specifically rather
// than taking a generic key_len to keep the surface small and the
// callers' intent unambiguous. If a future epic needs HMAC over a
// different key size, generalise then - not now.
//
// Output is truncated to 8 bytes, matching the Epic 21 wire frame
// trailer (CONFIG_WRITE.hmac[8]). The hash construction itself
// computes the full 32-byte HMAC-SHA256 and we memcpy the leading
// 8 bytes; this is a standard truncation per RFC 2104 §5.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace crypto {

// Compute HMAC-SHA256(key, msg) and write the leading 8 bytes to
// `out_mac8`. The remaining 24 bytes of the full HMAC are discarded.
void hmac_sha256_sign_t8(const uint8_t key[16],
                         const uint8_t* msg, size_t msg_len,
                         uint8_t out_mac8[8]);

// Verify `mac8` against HMAC-SHA256(key, msg) truncated to 8 bytes.
// Returns true iff they match. The comparison is constant-time with
// respect to `mac8` to deny timing-oracle leaks on the HMAC bytes.
bool hmac_sha256_verify_t8(const uint8_t key[16],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t mac8[8]);

}  // namespace crypto
}  // namespace nocturnation
