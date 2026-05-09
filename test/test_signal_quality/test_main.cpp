// Native unit tests for transport::SignalQuality.
//
// Coverage:
//   - Empty / pre-min-data state returns -1
//   - Lossless sequence -> 4 bars once min-frames threshold crossed
//   - Loss bands map to the documented bar counts
//   - seq=0 frames are skipped (sequencing disabled per spec §4.3)
//   - Source change resets state
//   - Wrap from 255 -> 1 doesn't synthesise phantom missed frames
//   - Window decay halves counters after kWindowMs

#include <unity.h>
#include "transport/quality.h"

using namespace nocturnation::transport;

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Threshold / state-progression tests
// ---------------------------------------------------------------------------

static void test_empty_returns_insufficient_data(void) {
    SignalQuality q;
    TEST_ASSERT_EQUAL_INT(-1, q.bars(0));
}

static void test_below_min_frames_returns_insufficient(void) {
    SignalQuality q;
    q.note_frame(/*src=*/1, /*seq=*/1, /*now=*/0);
    q.note_frame(1, 2, 10);
    // kMinFramesForBars = 3, only 2 received -> still -1
    TEST_ASSERT_EQUAL_INT(-1, q.bars(20));
}

static void test_lossless_three_frames_full_bars(void) {
    SignalQuality q;
    q.note_frame(1, 1, 0);
    q.note_frame(1, 2, 10);
    q.note_frame(1, 3, 20);
    TEST_ASSERT_EQUAL_INT(4, q.bars(30));
    TEST_ASSERT_EQUAL_UINT(3, q.received_count());
    TEST_ASSERT_EQUAL_UINT(0, q.missed_count());
}

static void test_one_missed_in_six_drops_to_two_bars(void) {
    // 5 received + 1 missed = 16.6% loss. Lands in the 15-30% band -> 2 bars.
    SignalQuality q;
    q.note_frame(1, 1, 0);
    q.note_frame(1, 2, 10);
    q.note_frame(1, 4, 30);  // missed seq 3
    q.note_frame(1, 5, 40);
    q.note_frame(1, 6, 50);
    TEST_ASSERT_EQUAL_UINT(5, q.received_count());
    TEST_ASSERT_EQUAL_UINT(1, q.missed_count());
    TEST_ASSERT_EQUAL_INT(2, q.bars(60));
}

static void test_one_missed_in_eleven_keeps_three_bars(void) {
    // 10 received + 1 missed = 9.1% loss. Lands in the 5-15% band -> 3 bars.
    SignalQuality q;
    q.note_frame(1, 1, 0);
    q.note_frame(1, 2, 10);
    q.note_frame(1, 3, 20);
    q.note_frame(1, 4, 30);
    q.note_frame(1, 5, 40);
    q.note_frame(1, 6, 50);
    q.note_frame(1, 7, 60);
    q.note_frame(1, 9, 80);  // missed seq 8
    q.note_frame(1, 10, 90);
    q.note_frame(1, 11, 100);
    q.note_frame(1, 12, 110);
    TEST_ASSERT_EQUAL_UINT(11, q.received_count());
    TEST_ASSERT_EQUAL_UINT(1,  q.missed_count());
    TEST_ASSERT_EQUAL_INT(3, q.bars(120));
}

static void test_severe_loss_zero_bars(void) {
    // 3 received + 5 missed = 62.5% loss. >=50% -> 0 bars.
    SignalQuality q;
    q.note_frame(1, 1, 0);
    q.note_frame(1, 4, 30);  // missed 2, 3
    q.note_frame(1, 8, 70);  // missed 5, 6, 7
    TEST_ASSERT_EQUAL_INT(0, q.bars(80));
}

// ---------------------------------------------------------------------------
// Special-value handling
// ---------------------------------------------------------------------------

static void test_seq_zero_skipped(void) {
    SignalQuality q;
    // Seq=0 frames represent "sequencing disabled" - shouldn't update state.
    q.note_frame(1, 0, 0);
    q.note_frame(1, 0, 10);
    q.note_frame(1, 0, 20);
    TEST_ASSERT_EQUAL_UINT(0, q.received_count());
    TEST_ASSERT_EQUAL_INT(-1, q.bars(30));
}

static void test_source_change_resets_state(void) {
    SignalQuality q;
    q.note_frame(/*src=*/1, 5, 0);
    q.note_frame(1, 6, 10);
    q.note_frame(1, 7, 20);
    TEST_ASSERT_EQUAL_INT(4, q.bars(30));

    // New source should reset; first frame from new source seeds without
    // synthesising missed frames from a now-defunct old master.
    q.note_frame(/*src=*/2, 100, 100);
    TEST_ASSERT_EQUAL_UINT(1, q.received_count());
    TEST_ASSERT_EQUAL_UINT(0, q.missed_count());
    TEST_ASSERT_EQUAL_INT(2, q.current_source());
}

static void test_wrap_from_255_to_1_no_phantom_missed(void) {
    // 255 -> 1 is a single legitimate step (sequence 0 is reserved).
    // We treat this as "wrap or out-of-order" and count 0 missed.
    SignalQuality q;
    q.note_frame(1, 254, 0);
    q.note_frame(1, 255, 10);
    q.note_frame(1, 1,   20);
    q.note_frame(1, 2,   30);
    TEST_ASSERT_EQUAL_UINT(4, q.received_count());
    TEST_ASSERT_EQUAL_UINT(0, q.missed_count());
    TEST_ASSERT_EQUAL_INT(4, q.bars(40));
}

// ---------------------------------------------------------------------------
// Window decay
// ---------------------------------------------------------------------------

static void test_window_decay_halves_counters(void) {
    SignalQuality q;
    // Three frames in the first window
    q.note_frame(1, 1, 0);
    q.note_frame(1, 2, 100);
    q.note_frame(1, 4, 200);  // 1 missed
    TEST_ASSERT_EQUAL_UINT(3, q.received_count());
    TEST_ASSERT_EQUAL_UINT(1, q.missed_count());

    // A frame arriving past the kWindowMs boundary triggers the halve-and-
    // restart. received: (3+1)/2 = 2, missed: 1/2 = 0, plus this new frame.
    const uint32_t after_window = SignalQuality::kWindowMs + 1000;
    q.note_frame(1, 5, after_window);
    TEST_ASSERT_EQUAL_UINT(3, q.received_count());   // 2 + 1 new
    TEST_ASSERT_EQUAL_UINT(0, q.missed_count());
}

static void test_reset_clears_everything(void) {
    SignalQuality q;
    q.note_frame(1, 1, 0);
    q.note_frame(1, 5, 50);  // pretend several missed
    q.reset();
    TEST_ASSERT_EQUAL_UINT(0, q.received_count());
    TEST_ASSERT_EQUAL_UINT(0, q.missed_count());
    TEST_ASSERT_EQUAL_UINT(0, q.current_source());
    TEST_ASSERT_EQUAL_INT(-1, q.bars(100));
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_returns_insufficient_data);
    RUN_TEST(test_below_min_frames_returns_insufficient);
    RUN_TEST(test_lossless_three_frames_full_bars);
    RUN_TEST(test_one_missed_in_six_drops_to_two_bars);
    RUN_TEST(test_one_missed_in_eleven_keeps_three_bars);
    RUN_TEST(test_severe_loss_zero_bars);
    RUN_TEST(test_seq_zero_skipped);
    RUN_TEST(test_source_change_resets_state);
    RUN_TEST(test_wrap_from_255_to_1_no_phantom_missed);
    RUN_TEST(test_window_decay_halves_counters);
    RUN_TEST(test_reset_clears_everything);
    return UNITY_END();
}
