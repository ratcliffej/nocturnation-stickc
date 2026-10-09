// Pairing register tests (Epic 21 B5).
//
// Covers:
//   - Empty register starts empty.
//   - add() stores an entry; find() returns it.
//   - add() on an existing UID updates in place (preserves
//     last_configured_at, bumps captured_at).
//   - remove() takes it out; find() returns null.
//   - Saturation at kMaxEntries evicts the oldest-by-captured_at.
//   - list_sorted_by_captured_at returns most-recent-first.
//   - mark_configured updates last_configured_at.
//   - save() / load() round-trip via the native NVS stub preserves
//     every entry byte-for-byte + the monotonic sequence.

#include <unity.h>
#include <cstdint>
#include <cstring>

#include "../../src/ble/pair_register.h"

using namespace nocturnation::ble;

// Native-only test seam from pair_register.cpp.
namespace nocturnation { namespace ble { void test_reset_native_pair_blob(); } }

void setUp(void) {
    nocturnation::ble::test_reset_native_pair_blob();
}
void tearDown(void) {}

static RegisterEntry make_entry(uint32_t uid, const char* name = "") {
    RegisterEntry e = {};
    e.uid = uid;
    for (size_t i = 0; i < 16; ++i) e.secret[i] = static_cast<uint8_t>(0x80 ^ i ^ (uid & 0xFF));
    e.role = 0x02;
    e.host = 0x03;
    size_t n = 0;
    while (name[n] && n < 20) { e.friendly_name[n] = name[n]; ++n; }
    e.friendly_name[n] = '\0';
    return e;
}

void test_empty_register_has_no_entries() {
    PairRegister r;
    TEST_ASSERT_EQUAL_size_t(0, r.count());
    TEST_ASSERT_NULL(r.find(0x12345678u));
}

void test_add_and_find_round_trip() {
    PairRegister r;
    TEST_ASSERT_TRUE(r.add(make_entry(0xAAAAu, "Alpha")));
    TEST_ASSERT_EQUAL_size_t(1, r.count());
    const auto* e = r.find(0xAAAAu);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(0xAAAAu, e->uid);
    TEST_ASSERT_EQUAL_STRING("Alpha", e->friendly_name);
    TEST_ASSERT_GREATER_THAN_UINT32(0, e->captured_at);
}

void test_re_add_updates_in_place_and_bumps_captured_at() {
    PairRegister r;
    r.add(make_entry(0x1111u, "first"));
    const uint32_t first_captured = r.find(0x1111u)->captured_at;
    r.add(make_entry(0x2222u));          // bump sequence
    r.add(make_entry(0x1111u, "second"));
    TEST_ASSERT_EQUAL_size_t(2, r.count());
    const auto* e = r.find(0x1111u);
    TEST_ASSERT_EQUAL_STRING("second", e->friendly_name);
    TEST_ASSERT_GREATER_THAN_UINT32(first_captured, e->captured_at);
}

void test_remove_takes_entry_out() {
    PairRegister r;
    r.add(make_entry(0x1111u, "a"));
    r.add(make_entry(0x2222u, "b"));
    TEST_ASSERT_TRUE(r.remove(0x1111u));
    TEST_ASSERT_EQUAL_size_t(1, r.count());
    TEST_ASSERT_NULL(r.find(0x1111u));
    TEST_ASSERT_NOT_NULL(r.find(0x2222u));
    TEST_ASSERT_FALSE(r.remove(0x9999u));   // not present
}

void test_saturation_evicts_oldest_by_captured_at() {
    PairRegister r;
    for (uint32_t i = 1; i <= PairRegister::kMaxEntries; ++i) {
        r.add(make_entry(i));
    }
    TEST_ASSERT_EQUAL_size_t(PairRegister::kMaxEntries, r.count());
    // UID 1 was added first → smallest captured_at. Adding one more
    // new UID should evict UID 1.
    r.add(make_entry(0xFFFF0001u));
    TEST_ASSERT_EQUAL_size_t(PairRegister::kMaxEntries, r.count());
    TEST_ASSERT_NULL(r.find(1u));                 // evicted
    TEST_ASSERT_NOT_NULL(r.find(0xFFFF0001u));    // present
    TEST_ASSERT_NOT_NULL(r.find(2u));             // still there
}

void test_list_sorted_returns_most_recent_first() {
    PairRegister r;
    r.add(make_entry(0x1111u, "first"));
    r.add(make_entry(0x2222u, "second"));
    r.add(make_entry(0x3333u, "third"));
    RegisterEntry out[8] = {};
    const size_t n = r.list_sorted_by_captured_at(out, 8);
    TEST_ASSERT_EQUAL_size_t(3, n);
    TEST_ASSERT_EQUAL_UINT32(0x3333u, out[0].uid);
    TEST_ASSERT_EQUAL_UINT32(0x2222u, out[1].uid);
    TEST_ASSERT_EQUAL_UINT32(0x1111u, out[2].uid);
}

void test_mark_configured_sets_last_configured_at() {
    PairRegister r;
    r.add(make_entry(0x1111u));
    TEST_ASSERT_EQUAL_UINT32(0, r.find(0x1111u)->last_configured_at);
    TEST_ASSERT_TRUE(r.mark_configured(0x1111u));
    TEST_ASSERT_GREATER_THAN_UINT32(0, r.find(0x1111u)->last_configured_at);
    TEST_ASSERT_FALSE(r.mark_configured(0x9999u));
}

void test_mark_configured_preserved_across_re_add() {
    PairRegister r;
    r.add(make_entry(0x1111u, "first"));
    r.mark_configured(0x1111u);
    const uint32_t configured_before = r.find(0x1111u)->last_configured_at;
    TEST_ASSERT_GREATER_THAN_UINT32(0, configured_before);
    r.add(make_entry(0x1111u, "second"));            // re-pair same UID
    const auto* e = r.find(0x1111u);
    TEST_ASSERT_EQUAL_STRING("second", e->friendly_name);
    TEST_ASSERT_EQUAL_UINT32(configured_before, e->last_configured_at);
}

void test_save_load_round_trip_preserves_everything() {
    {
        PairRegister r;
        r.add(make_entry(0x1111u, "Front"));
        r.add(make_entry(0x2222u, "Back"));
        r.mark_configured(0x1111u);
        // Destructor doesn't save; add() + mark_configured() already did.
    }
    // Second instance loads from the native NVS blob.
    PairRegister r2;
    TEST_ASSERT_EQUAL_size_t(2, r2.count());
    const auto* a = r2.find(0x1111u);
    const auto* b = r2.find(0x2222u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_STRING("Front", a->friendly_name);
    TEST_ASSERT_EQUAL_STRING("Back",  b->friendly_name);
    TEST_ASSERT_GREATER_THAN_UINT32(0, a->last_configured_at);
    TEST_ASSERT_EQUAL_UINT32(0, b->last_configured_at);
    // Adding a new entry after reload bumps the sequence monotonically
    // from the loaded value — so the new entry's captured_at is
    // greater than any pre-reload captured_at.
    r2.add(make_entry(0x3333u, "New"));
    TEST_ASSERT_GREATER_THAN_UINT32(a->captured_at, r2.find(0x3333u)->captured_at);
    TEST_ASSERT_GREATER_THAN_UINT32(b->captured_at, r2.find(0x3333u)->captured_at);
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_register_has_no_entries);
    RUN_TEST(test_add_and_find_round_trip);
    RUN_TEST(test_re_add_updates_in_place_and_bumps_captured_at);
    RUN_TEST(test_remove_takes_entry_out);
    RUN_TEST(test_saturation_evicts_oldest_by_captured_at);
    RUN_TEST(test_list_sorted_returns_most_recent_first);
    RUN_TEST(test_mark_configured_sets_last_configured_at);
    RUN_TEST(test_mark_configured_preserved_across_re_add);
    RUN_TEST(test_save_load_round_trip_preserves_everything);
    return UNITY_END();
}
