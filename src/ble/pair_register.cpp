// Pairing register implementation — see pair_register.h.

#include "ble/pair_register.h"

#include <cstring>

#ifdef ARDUINO
#include <Preferences.h>
#endif

namespace nocturnation {
namespace ble {

namespace {

// On-NVS blob layout: entry_count (u8) + next_sequence_val (u32)
// + packed array of RegisterEntry. Header lets us detect "no register
// yet" cheaply (count = 0 or blob missing entirely) and preserves
// the monotonic sequence across reboots.
constexpr const char* kBlobKey = "pair_reg";

#ifdef ARDUINO
constexpr size_t kBlobHeaderSize = 1 + 4;   // count + next_sequence
#endif

}  // namespace

PairRegister::PairRegister() {
    load();
}

uint32_t PairRegister::next_sequence_() {
    const uint32_t v = next_sequence_val_;
    // Clamp at near-max so a very long-lived register never wraps to 0
    // (which would conflict with the "never configured" sentinel).
    if (next_sequence_val_ < 0xFFFFFFF0u) {
        ++next_sequence_val_;
    }
    return v;
}

bool PairRegister::add(const RegisterEntry& in) {
    // In-place update if the UID already exists. Preserves the
    // original captured_at (FIFO semantics - re-pairing doesn't
    // promote an entry; the eviction order stays "oldest insertion
    // first" however many times you re-pair). Also preserves
    // last_configured_at so a re-pair doesn't forget prior writes.
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].uid == in.uid) {
            const uint32_t prev_captured         = entries_[i].captured_at;
            const uint32_t prev_last_configured  = entries_[i].last_configured_at;
            entries_[i] = in;
            entries_[i].captured_at        = prev_captured;
            entries_[i].last_configured_at = prev_last_configured;
            save();
            return true;
        }
    }

    // New UID — append or evict-FIFO + append.
    size_t slot = count_;
    if (count_ >= kMaxEntries) {
        // FIFO: evict the entry with the smallest captured_at
        // (= longest-ago insertion). Captured_at is monotonic and
        // never updated after initial insertion, so this is
        // genuinely "first in, first out" regardless of any
        // subsequent re-pairs or configure writes.
        size_t   oldest_idx = 0;
        uint32_t oldest_seq = entries_[0].captured_at;
        for (size_t i = 1; i < kMaxEntries; ++i) {
            if (entries_[i].captured_at < oldest_seq) {
                oldest_seq = entries_[i].captured_at;
                oldest_idx = i;
            }
        }
        slot = oldest_idx;
    } else {
        ++count_;
    }

    entries_[slot] = in;
    entries_[slot].captured_at = next_sequence_();
    // Preserve caller's last_configured_at (defaults to 0 for a fresh
    // entry; test seam / explicit callers can set it).
    save();
    return true;
}

bool PairRegister::remove(uint32_t uid) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].uid == uid) {
            // Compact down over the removed slot.
            for (size_t j = i + 1; j < count_; ++j) {
                entries_[j - 1] = entries_[j];
            }
            --count_;
            entries_[count_] = {};
            save();
            return true;
        }
    }
    return false;
}

const RegisterEntry* PairRegister::find(uint32_t uid) const {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].uid == uid) return &entries_[i];
    }
    return nullptr;
}

size_t PairRegister::list_sorted_by_captured_at(RegisterEntry* out, size_t cap) const {
    const size_t n = (count_ < cap) ? count_ : cap;
    for (size_t i = 0; i < n; ++i) out[i] = entries_[i];
    // In-place insertion sort by captured_at descending (small N; simple).
    for (size_t i = 1; i < n; ++i) {
        RegisterEntry key = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].captured_at < key.captured_at) {
            out[j] = out[j - 1];
            --j;
        }
        out[j] = key;
    }
    return n;
}

bool PairRegister::mark_configured(uint32_t uid) {
    for (size_t i = 0; i < count_; ++i) {
        if (entries_[i].uid == uid) {
            entries_[i].last_configured_at = next_sequence_();
            save();
            return true;
        }
    }
    return false;
}

void PairRegister::clear_in_memory() {
    for (size_t i = 0; i < kMaxEntries; ++i) entries_[i] = {};
    count_ = 0;
    next_sequence_val_ = 1;
}

#ifdef ARDUINO

void PairRegister::load() {
    clear_in_memory();
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    const size_t blob_len = prefs.getBytesLength(kBlobKey);
    if (blob_len < kBlobHeaderSize) {
        prefs.end();
        return;
    }
    // Max possible blob size.
    uint8_t raw[kBlobHeaderSize + sizeof(RegisterEntry) * kMaxEntries];
    const size_t cap = sizeof(raw);
    const size_t to_read = blob_len < cap ? blob_len : cap;
    prefs.getBytes(kBlobKey, raw, to_read);
    prefs.end();

    const uint8_t  count = raw[0];
    const uint32_t seq   = (uint32_t)raw[1]
                         | ((uint32_t)raw[2] << 8)
                         | ((uint32_t)raw[3] << 16)
                         | ((uint32_t)raw[4] << 24);
    const size_t expected = kBlobHeaderSize + (size_t)count * sizeof(RegisterEntry);
    if (count > kMaxEntries || to_read < expected) {
        // Corrupt or truncated — leave empty.
        return;
    }
    count_             = count;
    next_sequence_val_ = seq ? seq : 1;
    std::memcpy(entries_, raw + kBlobHeaderSize,
                (size_t)count * sizeof(RegisterEntry));
}

void PairRegister::save() const {
    const size_t blob_len = kBlobHeaderSize + count_ * sizeof(RegisterEntry);
    uint8_t raw[kBlobHeaderSize + sizeof(RegisterEntry) * kMaxEntries];
    raw[0] = static_cast<uint8_t>(count_);
    raw[1] = static_cast<uint8_t>( next_sequence_val_        & 0xFF);
    raw[2] = static_cast<uint8_t>((next_sequence_val_ >>  8) & 0xFF);
    raw[3] = static_cast<uint8_t>((next_sequence_val_ >> 16) & 0xFF);
    raw[4] = static_cast<uint8_t>((next_sequence_val_ >> 24) & 0xFF);
    std::memcpy(raw + kBlobHeaderSize, entries_,
                count_ * sizeof(RegisterEntry));
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBytes(kBlobKey, raw, blob_len);
    prefs.end();
}

#else   // !ARDUINO: native stub, process-static buffer.

namespace {
uint8_t  s_native_pair_blob[1 + 4 + sizeof(RegisterEntry) * PairRegister::kMaxEntries] = {};
size_t   s_native_pair_blob_len = 0;
}  // namespace

void PairRegister::load() {
    clear_in_memory();
    if (s_native_pair_blob_len < 5) return;
    const uint8_t count = s_native_pair_blob[0];
    const uint32_t seq  = (uint32_t)s_native_pair_blob[1]
                         | ((uint32_t)s_native_pair_blob[2] << 8)
                         | ((uint32_t)s_native_pair_blob[3] << 16)
                         | ((uint32_t)s_native_pair_blob[4] << 24);
    if (count > kMaxEntries) return;
    count_             = count;
    next_sequence_val_ = seq ? seq : 1;
    std::memcpy(entries_, s_native_pair_blob + 5,
                (size_t)count * sizeof(RegisterEntry));
}

void PairRegister::save() const {
    s_native_pair_blob[0] = static_cast<uint8_t>(count_);
    s_native_pair_blob[1] = static_cast<uint8_t>( next_sequence_val_        & 0xFF);
    s_native_pair_blob[2] = static_cast<uint8_t>((next_sequence_val_ >>  8) & 0xFF);
    s_native_pair_blob[3] = static_cast<uint8_t>((next_sequence_val_ >> 16) & 0xFF);
    s_native_pair_blob[4] = static_cast<uint8_t>((next_sequence_val_ >> 24) & 0xFF);
    std::memcpy(s_native_pair_blob + 5, entries_,
                count_ * sizeof(RegisterEntry));
    s_native_pair_blob_len = 5 + count_ * sizeof(RegisterEntry);
}

// Test seam exposed for native tests only — reset the NVS stub.
void test_reset_native_pair_blob() {
    std::memset(s_native_pair_blob, 0, sizeof(s_native_pair_blob));
    s_native_pair_blob_len = 0;
}

#endif

PairRegister& pair_register() {
    static PairRegister instance;
    return instance;
}

}  // namespace ble
}  // namespace nocturnation
