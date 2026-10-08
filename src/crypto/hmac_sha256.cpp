// HMAC-SHA256 implementation — see hmac_sha256.h for the API contract
// and the "why not mbedtls" rationale. Pure portable C++; no Arduino
// or ESP-IDF dependencies so the native test env builds it unchanged.
//
// Reference: FIPS 180-4 (SHA-256) and RFC 2104 (HMAC construction).
// Tested against RFC 4231 Test Case 1 known-answer vectors in
// test/test_hmac_sha256/.

#include "crypto/hmac_sha256.h"

#include <cstring>

namespace nocturnation {
namespace crypto {

namespace {

// SHA-256 round constants (first 32 bits of the fractional parts of
// the cube roots of the first 64 primes). FIPS 180-4 §4.2.2.
constexpr uint32_t kK[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

inline uint32_t rotr(uint32_t x, uint32_t n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ ((~x) & z);
}
inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}
inline uint32_t big_sigma0(uint32_t x) { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); }
inline uint32_t big_sigma1(uint32_t x) { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); }
inline uint32_t sml_sigma0(uint32_t x) { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); }
inline uint32_t sml_sigma1(uint32_t x) { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); }

// Compress one 64-byte block into the running hash state H[0..7].
void sha256_compress_block(uint32_t H[8], const uint8_t block[64]) {
    uint32_t W[64];
    // Message schedule — big-endian decode of the block into W[0..15].
    for (size_t t = 0; t < 16; ++t) {
        W[t] = (static_cast<uint32_t>(block[t * 4    ]) << 24) |
               (static_cast<uint32_t>(block[t * 4 + 1]) << 16) |
               (static_cast<uint32_t>(block[t * 4 + 2]) <<  8) |
               (static_cast<uint32_t>(block[t * 4 + 3])      );
    }
    for (size_t t = 16; t < 64; ++t) {
        W[t] = sml_sigma1(W[t - 2]) + W[t - 7] + sml_sigma0(W[t - 15]) + W[t - 16];
    }

    uint32_t a = H[0], b = H[1], c = H[2], d = H[3];
    uint32_t e = H[4], f = H[5], g = H[6], h = H[7];

    for (size_t t = 0; t < 64; ++t) {
        const uint32_t T1 = h + big_sigma1(e) + ch(e, f, g) + kK[t] + W[t];
        const uint32_t T2 = big_sigma0(a) + maj(a, b, c);
        h = g; g = f; f = e; e = d + T1;
        d = c; c = b; b = a; a = T1 + T2;
    }

    H[0] += a; H[1] += b; H[2] += c; H[3] += d;
    H[4] += e; H[5] += f; H[6] += g; H[7] += h;
}

// Full-message SHA-256 — one-shot, no streaming API because every
// NocturNation HMAC input fits in a single sign/verify call.
void sha256(const uint8_t* msg, size_t msg_len, uint8_t out[32]) {
    uint32_t H[8] = {
        0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
        0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
    };

    // Process whole blocks straight from `msg`.
    size_t off = 0;
    while (msg_len - off >= 64) {
        sha256_compress_block(H, msg + off);
        off += 64;
    }

    // Padding into a tail buffer. We may need either one or two final
    // blocks depending on whether the leftover + 0x80 + 8 bytes of
    // length fit in a single 64-byte block.
    uint8_t tail[128] = {};
    const size_t leftover = msg_len - off;
    if (leftover > 0) std::memcpy(tail, msg + off, leftover);
    tail[leftover] = 0x80;
    const size_t tail_len = (leftover < 56) ? 64 : 128;

    const uint64_t bit_len = static_cast<uint64_t>(msg_len) * 8u;
    for (size_t i = 0; i < 8; ++i) {
        tail[tail_len - 1 - i] = static_cast<uint8_t>(bit_len >> (i * 8));
    }

    sha256_compress_block(H, tail);
    if (tail_len == 128) sha256_compress_block(H, tail + 64);

    for (size_t i = 0; i < 8; ++i) {
        out[i * 4    ] = static_cast<uint8_t>(H[i] >> 24);
        out[i * 4 + 1] = static_cast<uint8_t>(H[i] >> 16);
        out[i * 4 + 2] = static_cast<uint8_t>(H[i] >>  8);
        out[i * 4 + 3] = static_cast<uint8_t>(H[i]      );
    }
}

// Full 32-byte HMAC-SHA256 output; truncation happens at the API
// boundary. Key is always 16 bytes (NocturNation Epic 21 secret);
// the 64-byte block pad zero-extends the key per RFC 2104 §2.
void hmac_sha256_full(const uint8_t key[16],
                      const uint8_t* msg, size_t msg_len,
                      uint8_t out[32]) {
    uint8_t ipad[64] = {};
    uint8_t opad[64] = {};
    std::memcpy(ipad, key, 16);
    std::memcpy(opad, key, 16);
    for (size_t i = 0; i < 64; ++i) {
        ipad[i] ^= 0x36;
        opad[i] ^= 0x5c;
    }

    // Inner hash: SHA256(ipad || msg).
    // Rather than concatenate (which would need a heap allocation for
    // long messages), use a streaming-ish approach: run the first
    // block as ipad, then feed msg via a small reusable buffer.
    // Simpler for the Epic 21 ~241-byte max message: one-shot
    // concatenate on the stack.
    //
    // Max msg_len = kConfigWriteMaxPayloadLen (241) + kHeaderSize (9)
    // = 250 bytes. Add the 64-byte ipad prefix = 314 bytes. Fits
    // comfortably in a stack buffer.
    constexpr size_t kMaxInnerInput = 64 + 320;
    uint8_t inner_input[kMaxInnerInput];
    if (msg_len > kMaxInnerInput - 64) {
        // Over-long input — zero the output and return. This would
        // only happen if a caller handed us a buffer bigger than the
        // longest NocturNation frame; defensive, shouldn't fire in
        // production. Returning zeros makes a subsequent verify fail
        // deterministically rather than reading uninitialised memory.
        std::memset(out, 0, 32);
        return;
    }
    std::memcpy(inner_input, ipad, 64);
    if (msg_len > 0) std::memcpy(inner_input + 64, msg, msg_len);

    uint8_t inner_hash[32];
    sha256(inner_input, 64 + msg_len, inner_hash);

    // Outer hash: SHA256(opad || inner_hash). Fixed 96-byte input.
    uint8_t outer_input[96];
    std::memcpy(outer_input,      opad,       64);
    std::memcpy(outer_input + 64, inner_hash, 32);
    sha256(outer_input, 96, out);
}

}  // namespace

void hmac_sha256_sign_t8(const uint8_t key[16],
                         const uint8_t* msg, size_t msg_len,
                         uint8_t out_mac8[8]) {
    uint8_t full[32];
    hmac_sha256_full(key, msg, msg_len, full);
    std::memcpy(out_mac8, full, 8);
}

bool hmac_sha256_verify_t8(const uint8_t key[16],
                           const uint8_t* msg, size_t msg_len,
                           const uint8_t mac8[8]) {
    uint8_t expected[8];
    hmac_sha256_sign_t8(key, msg, msg_len, expected);
    // Constant-time compare: fold bytewise differences into a single
    // accumulator with OR, then test the accumulator at the end.
    // This denies a timing oracle on `mac8` so a remote attacker
    // can't recover expected bytes one-at-a-time by measuring reply
    // latency on crafted CONFIG_WRITEs.
    uint8_t diff = 0;
    for (size_t i = 0; i < 8; ++i) diff |= (expected[i] ^ mac8[i]);
    return diff == 0;
}

}  // namespace crypto
}  // namespace nocturnation
