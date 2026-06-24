// Host tests for TofuLock - the StickC port of the Tildagon's
// TofuLock. Mirrors the Python test suite at
// nocturnation-tildagon/tests/test_tofu.py so the two implementations
// stay behavioural-equivalent.

#include <cstdint>
#include <cstring>
#include <unity.h>

#include "transport/espnow/tofu_lock.h"

using nocturnation::transport::espnow::TofuLock;
using nocturnation::transport::espnow::format_tofu_lock_label;
using nocturnation::transport::espnow::MessageType;

void setUp(void) {}
void tearDown(void) {}

// =============================================================================
// Initial state
// =============================================================================

static void test_not_locked_on_construction(void) {
    TofuLock t;
    TEST_ASSERT_FALSE(t.is_locked());
}

// =============================================================================
// Lock acquisition
// =============================================================================

static void test_admits_first_frame_and_locks(void) {
    TofuLock t;
    const bool admitted = t.admit(MessageType::LightPulse,
                                  /*source_id=*/0x42, /*channel=*/1,
                                  /*now_ms=*/100);
    TEST_ASSERT_TRUE(admitted);
    TEST_ASSERT_TRUE(t.is_locked());
    TEST_ASSERT_EQUAL_UINT8(0x42, t.locked_id());
}

static void test_first_frame_can_be_light_pulse_not_only_heartbeat(void) {
    // Spec-relaxation note from Tildagon TofuLock docstring: the
    // Director's heartbeat is suppressed by skip-if-recent during
    // active music, so a Lume joining mid-song would otherwise sit
    // idle. Any valid frame must establish the lock, not just
    // HEARTBEAT.
    TofuLock t;
    const bool admitted = t.admit(MessageType::LightWash,
                                  0x05, /*channel=*/1, 100);
    TEST_ASSERT_TRUE(admitted);
    TEST_ASSERT_TRUE(t.is_locked());
}

static void test_broadcast_source_id_never_locks(void) {
    TofuLock t;
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0xFF, /*channel=*/1, 0);
    TEST_ASSERT_FALSE(admitted);
    TEST_ASSERT_FALSE(t.is_locked());
}

// =============================================================================
// Post-lock filter
// =============================================================================

static void test_admits_subsequent_frames_from_locked_source(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 100);
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0x05, /*channel=*/1, 200);
    TEST_ASSERT_TRUE(admitted);
}

static void test_drops_frames_from_different_source_after_lock(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 100);
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0x06, /*channel=*/1, 200);
    TEST_ASSERT_FALSE(admitted);
    TEST_ASSERT_EQUAL_UINT8(0x05, t.locked_id());   // unchanged
}

// =============================================================================
// Channel-11 cross-range filter
// =============================================================================

static void test_performance_range_locks_on_ch11(void) {
    TofuLock t;
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0x4F, /*channel=*/11, 0);
    TEST_ASSERT_TRUE(admitted);
    TEST_ASSERT_TRUE(t.is_locked());
}

static void test_community_range_dropped_on_ch11_without_lock(void) {
    TofuLock t;
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0x05, /*channel=*/11, 0);
    TEST_ASSERT_FALSE(admitted);
    TEST_ASSERT_FALSE(t.is_locked());
}

static void test_community_range_dropped_on_ch11_even_after_perf_lock(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x4F, /*channel=*/11, 0);
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0x05, /*channel=*/11, 100);
    TEST_ASSERT_FALSE(admitted);
    TEST_ASSERT_EQUAL_UINT8(0x4F, t.locked_id());
}

static void test_performance_min_max_lock_on_ch11(void) {
    TofuLock a; TEST_ASSERT_TRUE(a.admit(MessageType::LightPulse,
                                         0x40, /*channel=*/11, 0));
    TofuLock b; TEST_ASSERT_TRUE(b.admit(MessageType::LightPulse,
                                         0xFE, /*channel=*/11, 0));
}

static void test_community_range_locks_on_ch1(void) {
    TofuLock t;
    TEST_ASSERT_TRUE(t.admit(MessageType::LightPulse,
                             0x05, /*channel=*/1, 0));
}

static void test_performance_range_also_locks_on_ch1(void) {
    TofuLock t;
    TEST_ASSERT_TRUE(t.admit(MessageType::LightPulse,
                             0x4F, /*channel=*/1, 0));
}

static void test_any_non_broadcast_id_locks_on_ch6(void) {
    TofuLock t;
    TEST_ASSERT_TRUE(t.admit(MessageType::LightPulse,
                             0x10, /*channel=*/6, 0));
}

// =============================================================================
// Tick / inactivity timeout
// =============================================================================

static void test_tick_before_timeout_keeps_lock(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 0);
    const bool expired = t.tick(/*now_ms=*/TofuLock::kDefaultTimeoutMs - 1);
    TEST_ASSERT_FALSE(expired);
    TEST_ASSERT_TRUE(t.is_locked());
}

static void test_tick_past_timeout_expires_lock(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 0);
    const bool expired = t.tick(/*now_ms=*/TofuLock::kDefaultTimeoutMs);
    TEST_ASSERT_TRUE(expired);
    TEST_ASSERT_FALSE(t.is_locked());
    // Second tick at the same timestamp must not re-fire the edge.
    const bool expired2 = t.tick(/*now_ms=*/TofuLock::kDefaultTimeoutMs);
    TEST_ASSERT_FALSE(expired2);
}

static void test_admit_extends_timeout(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 0);
    // 9 s in - still locked
    TEST_ASSERT_FALSE(t.tick(9000));
    // A fresh admit() resets last_frame_ms_
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 9500);
    // Now 18 s in - WOULD have expired if admit() hadn't reset the
    // timer, but the new last_frame_ms_ = 9500 means age = 8500 < timeout.
    TEST_ASSERT_FALSE(t.tick(18000));
    TEST_ASSERT_TRUE(t.is_locked());
}

static void test_tick_on_unlocked_state_is_noop(void) {
    TofuLock t;
    TEST_ASSERT_FALSE(t.tick(60000));
    TEST_ASSERT_FALSE(t.is_locked());
}

// =============================================================================
// Explicit clear
// =============================================================================

static void test_clear_unlocks(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 0);
    TEST_ASSERT_TRUE(t.is_locked());
    t.clear();
    TEST_ASSERT_FALSE(t.is_locked());
}

static void test_after_clear_next_frame_locks_fresh(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x05, /*channel=*/1, 0);
    t.clear();
    t.admit(MessageType::LightPulse, 0x07, /*channel=*/1, 1000);
    TEST_ASSERT_TRUE(t.is_locked());
    TEST_ASSERT_EQUAL_UINT8(0x07, t.locked_id());
}

// =============================================================================
// Display-family broadcast admission (Epic 13)
// =============================================================================

static void test_text_display_broadcast_dropped_when_not_locked(void) {
    TofuLock t;
    const bool admitted = t.admit(MessageType::TextDisplay,
                                  0xFF, /*channel=*/1, 0);
    TEST_ASSERT_FALSE(admitted);
    TEST_ASSERT_FALSE(t.is_locked());
}

static void test_text_display_broadcast_admitted_when_locked(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x42, /*channel=*/1, 100);
    const bool admitted = t.admit(MessageType::TextDisplay,
                                  0xFF, /*channel=*/1, 200);
    TEST_ASSERT_TRUE(admitted);
    TEST_ASSERT_EQUAL_UINT8(0x42, t.locked_id());   // lock still names the Director
}

static void test_clear_screen_broadcast_admitted_when_locked(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x42, /*channel=*/1, 100);
    TEST_ASSERT_TRUE(t.admit(MessageType::ClearScreen,
                             0xFF, /*channel=*/1, 200));
}

static void test_bitmap_broadcasts_admitted_when_locked(void) {
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x42, /*channel=*/1, 100);
    TEST_ASSERT_TRUE(t.admit(MessageType::BitmapHeader,
                             0xFF, /*channel=*/1, 200));
    TEST_ASSERT_TRUE(t.admit(MessageType::BitmapPlane,
                             0xFF, /*channel=*/1, 201));
}

static void test_display_broadcast_does_not_reset_liveness_timer(void) {
    // Liveness is the LOCKED Director's responsibility. A display
    // broadcast must not extend the lock - otherwise a Director that
    // has gone silent could appear alive because the orchestrator is
    // still emitting display cues.
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x42, /*channel=*/1, 0);
    // Display broadcast at t=5 s: admitted, must not bump last_frame_ms.
    t.admit(MessageType::TextDisplay, 0xFF, /*channel=*/1, 5000);
    // tick at t=10.5 s: 10.5 s after the original Director frame -
    // past the 10 s timeout -> lock expires regardless of the
    // display traffic in between.
    TEST_ASSERT_TRUE(t.tick(10500));
    TEST_ASSERT_FALSE(t.is_locked());
}

static void test_non_display_broadcast_still_rejected_when_locked(void) {
    // LIGHT_PULSE from source_id 0xFF is misconfigured anonymous
    // traffic - reject even when a lock is held.
    TofuLock t;
    t.admit(MessageType::LightPulse, 0x42, /*channel=*/1, 100);
    const bool admitted = t.admit(MessageType::LightPulse,
                                  0xFF, /*channel=*/1, 200);
    TEST_ASSERT_FALSE(admitted);
}

// =============================================================================
// Status-label formatting
// =============================================================================

static void test_label_empty_when_unlocked(void) {
    char buf[8];
    const size_t n = format_tofu_lock_label(false, 0, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(0, n);
    TEST_ASSERT_EQUAL_STRING("", buf);
}

static void test_label_community_range_prefix(void) {
    char buf[8];
    format_tofu_lock_label(true, 0x3F, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("C:3F", buf);
}

static void test_label_performance_range_prefix(void) {
    char buf[8];
    format_tofu_lock_label(true, 0x4F, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("P:4F", buf);
}

static void test_label_broadcast_id_shows_question_prefix(void) {
    // Defensive - shouldn't happen for a conforming Director, but
    // the label surface stays informative if it does.
    char buf[8];
    format_tofu_lock_label(true, 0xFF, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_STRING("?:FF", buf);
}

// =============================================================================
// Entry point
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_not_locked_on_construction);
    RUN_TEST(test_admits_first_frame_and_locks);
    RUN_TEST(test_first_frame_can_be_light_pulse_not_only_heartbeat);
    RUN_TEST(test_broadcast_source_id_never_locks);
    RUN_TEST(test_admits_subsequent_frames_from_locked_source);
    RUN_TEST(test_drops_frames_from_different_source_after_lock);
    RUN_TEST(test_performance_range_locks_on_ch11);
    RUN_TEST(test_community_range_dropped_on_ch11_without_lock);
    RUN_TEST(test_community_range_dropped_on_ch11_even_after_perf_lock);
    RUN_TEST(test_performance_min_max_lock_on_ch11);
    RUN_TEST(test_community_range_locks_on_ch1);
    RUN_TEST(test_performance_range_also_locks_on_ch1);
    RUN_TEST(test_any_non_broadcast_id_locks_on_ch6);
    RUN_TEST(test_tick_before_timeout_keeps_lock);
    RUN_TEST(test_tick_past_timeout_expires_lock);
    RUN_TEST(test_admit_extends_timeout);
    RUN_TEST(test_tick_on_unlocked_state_is_noop);
    RUN_TEST(test_clear_unlocks);
    RUN_TEST(test_after_clear_next_frame_locks_fresh);
    RUN_TEST(test_text_display_broadcast_dropped_when_not_locked);
    RUN_TEST(test_text_display_broadcast_admitted_when_locked);
    RUN_TEST(test_clear_screen_broadcast_admitted_when_locked);
    RUN_TEST(test_bitmap_broadcasts_admitted_when_locked);
    RUN_TEST(test_display_broadcast_does_not_reset_liveness_timer);
    RUN_TEST(test_non_display_broadcast_still_rejected_when_locked);
    RUN_TEST(test_label_empty_when_unlocked);
    RUN_TEST(test_label_community_range_prefix);
    RUN_TEST(test_label_performance_range_prefix);
    RUN_TEST(test_label_broadcast_id_shows_question_prefix);
    return UNITY_END();
}
