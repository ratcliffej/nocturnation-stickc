// Numonce LRU tests (Epic 21 B3c).
//
// Covers:
//   - Fresh source accepted; same source + strictly-greater numonce
//     accepted; same source + equal / smaller numonce rejected.
//   - Numonce 0 rejected (reserved sentinel).
//   - 16-source saturation + 17th evicts the LRU slot.
//   - After eviction, the evicted source re-adds fresh (documented
//     v1 limitation; see numonce_lru.h header note).
//   - Access promotes LRU timestamp (so a frequently-touched slot
//     isn't the eviction target).

#include <unity.h>
#include <cstdint>

#include "../../src/transport/espnow/numonce_lru.h"

using namespace nocturnation::transport::espnow;

void setUp(void) {}
void tearDown(void) {}

void test_fresh_source_accepted() {
    NumonceLru lru;
    TEST_ASSERT_TRUE(lru.check_and_update(0x1234, 1));
    TEST_ASSERT_EQUAL_size_t(1, lru.slots_in_use());
    TEST_ASSERT_EQUAL_UINT64(1u, lru.last_seen_for(0x1234));
}

void test_strictly_greater_numonce_accepted() {
    NumonceLru lru;
    TEST_ASSERT_TRUE(lru.check_and_update(0x1234, 10));
    TEST_ASSERT_TRUE(lru.check_and_update(0x1234, 11));
    TEST_ASSERT_EQUAL_UINT64(11u, lru.last_seen_for(0x1234));
}

void test_equal_numonce_rejected() {
    NumonceLru lru;
    TEST_ASSERT_TRUE (lru.check_and_update(0x1234, 10));
    TEST_ASSERT_FALSE(lru.check_and_update(0x1234, 10));
    TEST_ASSERT_EQUAL_UINT64(10u, lru.last_seen_for(0x1234));
}

void test_smaller_numonce_rejected() {
    NumonceLru lru;
    TEST_ASSERT_TRUE (lru.check_and_update(0x1234, 10));
    TEST_ASSERT_FALSE(lru.check_and_update(0x1234, 5));
    TEST_ASSERT_EQUAL_UINT64(10u, lru.last_seen_for(0x1234));
}

void test_numonce_zero_rejected() {
    NumonceLru lru;
    TEST_ASSERT_FALSE(lru.check_and_update(0x1234, 0));
    TEST_ASSERT_EQUAL_size_t(0, lru.slots_in_use());
}

void test_multiple_sources_tracked_independently() {
    NumonceLru lru;
    TEST_ASSERT_TRUE(lru.check_and_update(0x0001, 100));
    TEST_ASSERT_TRUE(lru.check_and_update(0x0002, 50));
    TEST_ASSERT_TRUE(lru.check_and_update(0x0001, 101));
    TEST_ASSERT_TRUE(lru.check_and_update(0x0002, 51));
    TEST_ASSERT_EQUAL_UINT64(101u, lru.last_seen_for(0x0001));
    TEST_ASSERT_EQUAL_UINT64(51u,  lru.last_seen_for(0x0002));
    TEST_ASSERT_EQUAL_size_t(2, lru.slots_in_use());
}

void test_saturate_then_evict_lru() {
    NumonceLru lru;
    // Fill all 16 slots with distinct source_ids. Order: source_id = i+1,
    // numonce = 1. The first inserted (source_id=1) has the oldest
    // last_accessed timestamp.
    for (uint16_t i = 1; i <= NumonceLru::kSlots; ++i) {
        TEST_ASSERT_TRUE(lru.check_and_update(i, 1));
    }
    TEST_ASSERT_EQUAL_size_t(NumonceLru::kSlots, lru.slots_in_use());

    // 17th unique source triggers eviction. The LRU is source_id=1.
    TEST_ASSERT_TRUE(lru.check_and_update(100, 1));
    TEST_ASSERT_EQUAL_size_t(NumonceLru::kSlots, lru.slots_in_use());
    // source_id=1 is gone - last_seen_for returns 0.
    TEST_ASSERT_EQUAL_UINT64(0u, lru.last_seen_for(1));
    // source_id=100 is now tracked.
    TEST_ASSERT_EQUAL_UINT64(1u, lru.last_seen_for(100));
    // Other sources (2..16) still remembered.
    for (uint16_t i = 2; i <= NumonceLru::kSlots; ++i) {
        TEST_ASSERT_EQUAL_UINT64(1u, lru.last_seen_for(i));
    }
}

void test_evicted_source_rejoins_fresh() {
    NumonceLru lru;
    for (uint16_t i = 1; i <= NumonceLru::kSlots; ++i) {
        lru.check_and_update(i, 1000);   // high numonce
    }
    lru.check_and_update(100, 1);        // evicts source_id=1
    // source_id=1 comes back. Its previous last_seen=1000 is gone -
    // the LRU treats it as a brand-new source. numonce=1 is accepted.
    // This is the documented v1 limitation (see numonce_lru.h).
    TEST_ASSERT_TRUE(lru.check_and_update(1, 1));
    TEST_ASSERT_EQUAL_UINT64(1u, lru.last_seen_for(1));
}

void test_access_promotes_lru_timestamp() {
    NumonceLru lru;
    // Fill. source_id=1 has oldest access.
    for (uint16_t i = 1; i <= NumonceLru::kSlots; ++i) {
        lru.check_and_update(i, 1);
    }
    // Touch source_id=1 by sending a fresh numonce - this should
    // promote its LRU timestamp above source_id=2's.
    TEST_ASSERT_TRUE(lru.check_and_update(1, 2));
    // Now a 17th unique source evicts source_id=2 (now the oldest),
    // NOT source_id=1 (just promoted).
    TEST_ASSERT_TRUE(lru.check_and_update(100, 1));
    TEST_ASSERT_EQUAL_UINT64(2u, lru.last_seen_for(1));    // still here
    TEST_ASSERT_EQUAL_UINT64(0u, lru.last_seen_for(2));    // evicted
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_fresh_source_accepted);
    RUN_TEST(test_strictly_greater_numonce_accepted);
    RUN_TEST(test_equal_numonce_rejected);
    RUN_TEST(test_smaller_numonce_rejected);
    RUN_TEST(test_numonce_zero_rejected);
    RUN_TEST(test_multiple_sources_tracked_independently);
    RUN_TEST(test_saturate_then_evict_lru);
    RUN_TEST(test_evicted_source_rejoins_fresh);
    RUN_TEST(test_access_promotes_lru_timestamp);
    return UNITY_END();
}
