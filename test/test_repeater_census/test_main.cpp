// Host tests for RepeaterCensus - the Director-side headless-repeater tally.

#include <cstddef>
#include <cstdint>
#include <unity.h>

#include "dal/drivers/repeater_census.h"

using nocturnation::dal::RepeaterCensus;

void setUp(void) {}
void tearDown(void) {}

static void test_single_repeater_counts_online(void) {
    RepeaterCensus c;
    const uint8_t uid[3] = {0xAA, 0xBB, 0xCC};
    c.note(uid, 11, 5, 30, 1000);
    TEST_ASSERT_EQUAL_size_t(1, c.count_online(1000));
    // Still online at the edge of the window.
    TEST_ASSERT_EQUAL_size_t(
        1, c.count_online(1000 + RepeaterCensus::kOnlineWindowMs));
}

static void test_distinct_uids_counted_separately(void) {
    RepeaterCensus c;
    const uint8_t a[3] = {1, 2, 3};
    const uint8_t b[3] = {1, 2, 4};
    c.note(a, 1, 0, 0, 1000);
    c.note(b, 1, 0, 0, 1000);
    TEST_ASSERT_EQUAL_size_t(2, c.count_online(1000));
}

static void test_same_uid_updates_not_duplicates(void) {
    RepeaterCensus c;
    const uint8_t uid[3] = {9, 9, 9};
    c.note(uid, 1, 10, 5, 1000);
    c.note(uid, 6, 20, 6, 1500);   // same uid, fresh channel/relayed
    TEST_ASSERT_EQUAL_size_t(1, c.count_online(1500));

    bool found = false;
    for (size_t i = 0; i < RepeaterCensus::capacity(); ++i) {
        const RepeaterCensus::Entry& e = c.entries()[i];
        if (e.used && e.uid[0] == 9 && e.uid[1] == 9 && e.uid[2] == 9) {
            found = true;
            TEST_ASSERT_EQUAL_UINT8(6, e.channel);
            TEST_ASSERT_EQUAL_UINT32(20, e.relayed);
        }
    }
    TEST_ASSERT_TRUE(found);
}

static void test_stale_repeater_drops_off(void) {
    RepeaterCensus c;
    const uint8_t uid[3] = {7, 7, 7};
    c.note(uid, 11, 0, 0, 1000);
    const uint32_t stale = 1000 + RepeaterCensus::kOnlineWindowMs + 1;
    TEST_ASSERT_EQUAL_size_t(0, c.count_online(stale));
}

static void test_capacity_recycles_oldest(void) {
    RepeaterCensus c;
    // Register one more distinct repeater than the table holds, each
    // beacon a tick newer than the last, all inside the window.
    for (size_t i = 0; i <= RepeaterCensus::capacity(); ++i) {
        const uint8_t uid[3] = {static_cast<uint8_t>(i), 0, 0};
        c.note(uid, 1, 0, 0, static_cast<uint32_t>(1000 + i));
    }
    // Never exceeds capacity: the oldest was recycled, so exactly
    // `capacity` slots are used and all are within-window.
    const size_t online =
        c.count_online(static_cast<uint32_t>(1000 + RepeaterCensus::capacity()));
    TEST_ASSERT_EQUAL_size_t(RepeaterCensus::capacity(), online);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_single_repeater_counts_online);
    RUN_TEST(test_distinct_uids_counted_separately);
    RUN_TEST(test_same_uid_updates_not_duplicates);
    RUN_TEST(test_stale_repeater_drops_off);
    RUN_TEST(test_capacity_recycles_oldest);
    return UNITY_END();
}
