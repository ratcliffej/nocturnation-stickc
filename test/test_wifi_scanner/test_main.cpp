// Host tests for WifiScanner - aggregation + non-overlapping-channel
// picker. scan() itself is Arduino-only and not exercised here; these
// tests drive the aggregator via ingest_sample() with hand-picked
// fixtures so the picker's tie-break behaviour is deterministic.

#include <cstdint>
#include <unity.h>

#include "dal/drivers/wifi_scanner.h"

using nocturnation::dal::WifiScanner;
using nocturnation::dal::WifiChannelStats;

void setUp(void)    {}
void tearDown(void) {}

static void test_empty_scanner_recommends_channel_1(void) {
    WifiScanner s;
    TEST_ASSERT_EQUAL_UINT8(1, s.recommend_channel());
}

static void test_ingest_counts_and_max_rssi(void) {
    WifiScanner s;
    s.ingest_sample(6, -70);
    s.ingest_sample(6, -55);
    s.ingest_sample(6, -80);
    WifiChannelStats c6 = s.channel(6);
    TEST_ASSERT_EQUAL_UINT8(3,   c6.ap_count);
    TEST_ASSERT_EQUAL_INT8 (-55, c6.max_rssi);
}

static void test_out_of_range_channels_dropped(void) {
    WifiScanner s;
    s.ingest_sample(0,  -50);
    s.ingest_sample(14, -50);
    s.ingest_sample(36, -50);
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        TEST_ASSERT_EQUAL_UINT8(0, s.channel(ch).ap_count);
    }
}

static void test_recommend_picks_lowest_ap_count_from_1_6_11(void) {
    WifiScanner s;
    s.ingest_sample(1, -60); s.ingest_sample(1, -70); s.ingest_sample(1, -80);
    s.ingest_sample(6, -55); s.ingest_sample(6, -65); s.ingest_sample(6, -75);
    s.ingest_sample(6, -85); s.ingest_sample(6, -50);
    s.ingest_sample(11, -70);
    TEST_ASSERT_EQUAL_UINT8(11, s.recommend_channel());
}

static void test_recommend_never_picks_overlapping_channel(void) {
    WifiScanner s;
    s.ingest_sample(1,  -50); s.ingest_sample(1,  -60);
    s.ingest_sample(6,  -50); s.ingest_sample(6,  -60);
    s.ingest_sample(11, -50); s.ingest_sample(11, -60);
    const uint8_t rec = s.recommend_channel();
    TEST_ASSERT_TRUE(rec == 1 || rec == 6 || rec == 11);
}

static void test_recommend_tie_break_by_weakest_rssi(void) {
    WifiScanner s;
    s.ingest_sample(1,  -40);
    s.ingest_sample(6,  -85);
    s.ingest_sample(11, -60);
    TEST_ASSERT_EQUAL_UINT8(6, s.recommend_channel());
}

static void test_reset_clears_all_state(void) {
    WifiScanner s;
    s.ingest_sample(6,  -50);
    s.ingest_sample(11, -60);
    s.reset();
    for (uint8_t ch = 1; ch <= 13; ++ch) {
        TEST_ASSERT_EQUAL_UINT8(0, s.channel(ch).ap_count);
        TEST_ASSERT_EQUAL_INT8 (0, s.channel(ch).max_rssi);
    }
}

static void test_ingest_saturates_at_255(void) {
    WifiScanner s;
    for (int i = 0; i < 300; ++i) s.ingest_sample(1, -60);
    TEST_ASSERT_EQUAL_UINT8(255, s.channel(1).ap_count);
}

static void test_channel_out_of_range_returns_zero(void) {
    WifiScanner s;
    s.ingest_sample(6, -50);
    WifiChannelStats zero_a = s.channel(0);
    WifiChannelStats zero_b = s.channel(14);
    TEST_ASSERT_EQUAL_UINT8(0, zero_a.ap_count);
    TEST_ASSERT_EQUAL_INT8 (0, zero_a.max_rssi);
    TEST_ASSERT_EQUAL_UINT8(0, zero_b.ap_count);
    TEST_ASSERT_EQUAL_INT8 (0, zero_b.max_rssi);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_scanner_recommends_channel_1);
    RUN_TEST(test_ingest_counts_and_max_rssi);
    RUN_TEST(test_out_of_range_channels_dropped);
    RUN_TEST(test_recommend_picks_lowest_ap_count_from_1_6_11);
    RUN_TEST(test_recommend_never_picks_overlapping_channel);
    RUN_TEST(test_recommend_tie_break_by_weakest_rssi);
    RUN_TEST(test_reset_clears_all_state);
    RUN_TEST(test_ingest_saturates_at_255);
    RUN_TEST(test_channel_out_of_range_returns_zero);
    return UNITY_END();
}
