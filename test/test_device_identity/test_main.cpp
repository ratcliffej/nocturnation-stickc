// Device identity tests (Epic 21 B1).
//
// compute_uid_from_mac is the frozen contract between every Lume and
// every Director: a specific STA MAC must always produce a specific
// 32-bit UID, bit-identical on every host. These tests pin that.
//
// The reference vector was computed independently (Python
// `zlib.crc32(bytes([...]))`) and is the canary - if the vector
// changes, we have accidentally broken the UID contract and every
// deployed device would need re-pairing.
//
// ensure_identity / load_device_uid / load_device_secret are exercised
// against the native stub NVS in persistence.cpp.

#include <unity.h>
#include <cstring>

#include "../../src/modes/persistence.h"

using namespace nocturnation::modes::persistence;

// A reference MAC + its known-good CRC32 (reflected polynomial
// 0xEDB88320, final XOR 0xFFFFFFFF). Verified via
//   python3 -c "import zlib; print(hex(zlib.crc32(bytes([0x24,0x0A,0xC4,0x12,0xC1,0x58]))))"
// -> 0x3693cfa4. If this vector ever changes, every deployed Lume's
// UID would silently drift - don't touch without a wire-version bump.
constexpr uint8_t  kRefMac[6] = {0x24, 0x0A, 0xC4, 0x12, 0xC1, 0x58};
constexpr uint32_t kRefUid    = 0x3693CFA4u;

void test_compute_uid_matches_reference_vector() {
    const uint32_t uid = compute_uid_from_mac(kRefMac);
    TEST_ASSERT_EQUAL_HEX32(kRefUid, uid);
}

void test_compute_uid_is_deterministic() {
    const uint32_t a = compute_uid_from_mac(kRefMac);
    const uint32_t b = compute_uid_from_mac(kRefMac);
    TEST_ASSERT_EQUAL_HEX32(a, b);
}

void test_compute_uid_differs_for_different_mac() {
    const uint8_t mac2[6] = {0x24, 0x0A, 0xC4, 0x12, 0xC1, 0x59};   // one bit flipped
    const uint32_t a = compute_uid_from_mac(kRefMac);
    const uint32_t b = compute_uid_from_mac(mac2);
    TEST_ASSERT_NOT_EQUAL(a, b);
}

void test_ensure_identity_persists_what_it_wrote() {
    ensure_identity();
    const uint32_t uid1 = load_device_uid();
    TEST_ASSERT_NOT_EQUAL(0u, uid1);
    uint8_t secret1[16];
    load_device_secret(secret1);
    // Secret shouldn't be all-zero after ensure.
    bool any_nonzero = false;
    for (size_t i = 0; i < 16; ++i) if (secret1[i] != 0) { any_nonzero = true; break; }
    TEST_ASSERT_TRUE(any_nonzero);

    // Second call is a no-op - same values round-trip.
    ensure_identity();
    const uint32_t uid2 = load_device_uid();
    uint8_t secret2[16];
    load_device_secret(secret2);
    TEST_ASSERT_EQUAL_HEX32(uid1, uid2);
    TEST_ASSERT_EQUAL_MEMORY(secret1, secret2, 16);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_compute_uid_matches_reference_vector);
    RUN_TEST(test_compute_uid_is_deterministic);
    RUN_TEST(test_compute_uid_differs_for_different_mac);
    RUN_TEST(test_ensure_identity_persists_what_it_wrote);
    return UNITY_END();
}
