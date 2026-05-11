// EspNowBroadcastDriver implementation. Lifted from the per-mode
// EspNowBroadcaster helper (src/modes/espnow_broadcaster.{h,cpp}) in
// Epic 4.6 Block 2. Wire output, sequence numbering, source_id derivation,
// retransmit jitter and heartbeat skip-if-recent are preserved exactly.

#include "espnow_broadcast_driver.h"

#include "hal/hal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#endif

namespace nocturnation {
namespace dal {

namespace {
EspNowBroadcastDriver s_instance;

// now_ms() shim. Native test envs that link this TU (test_dal_*) don't
// pull in modes/ where mode_machine.cpp defines its native millis() seam,
// so we can't depend on millis() being available. Returning 0 in
// native builds means heartbeat / retransmit pacing doesn't advance
// under tests - fine, since the radio isn't active in those envs and
// active_ gates everything anyway.
inline uint32_t now_ms() {
#ifdef ARDUINO
    return ::millis();
#else
    return 0;
#endif
}
}  // namespace

EspNowBroadcastDriver* esp_now_broadcast_driver_instance() { return &s_instance; }

// -----------------------------------------------------------------------------
// Driver contract
// -----------------------------------------------------------------------------

bool EspNowBroadcastDriver::begin() {
    // Registration gate only - does NOT start the radio. Modes call
    // start_broadcast(channel) from enter() when they want airtime.
    return hal::HAL::esp_now() != nullptr;
}

void EspNowBroadcastDriver::loop_tick() {
    if (!active_) return;
    // Drain any pending redundant retransmits (per spec §4.3) and emit
    // the 1 Hz alive signal when no other frame has hit the wire recently.
    pump_retransmits();
    maybe_send_heartbeat();
}

bool EspNowBroadcastDriver::send(uint8_t group_id, const RgbPulseEvent& ev) {
    // Legacy path: target_class = All (0x00). The 3-arg overload below
    // is the canonical entry; this forwarder keeps existing call sites
    // (DAL legacy-name dispatch via the registered "esp-now-broadcast"
    // device) working with byte-identical wire output.
    return send(/*target_class=*/0, group_id, ev);
}

bool EspNowBroadcastDriver::send(uint8_t target_class,
                                  uint8_t target_group,
                                  const RgbPulseEvent& ev) {
    if (!active_) return false;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    LightCommandPayload p{};
    p.target_class = target_class;
    p.target_group = target_group;
    p.r = ev.r; p.g = ev.g; p.b = ev.b;
    p.attack  = static_cast<uint8_t>(ev.attack);
    p.sustain = static_cast<uint8_t>(ev.sustain);
    p.release = static_cast<uint8_t>(ev.release);
    p.chance  = static_cast<uint8_t>(ev.chance);
    uint8_t buf[kHeaderSize + kLightCommandPayloadLen];
    const size_t n = encode_light_command(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "LIGHT");
    return true;
}

// -----------------------------------------------------------------------------
// Driver-specific lifecycle / protocol entry points
// -----------------------------------------------------------------------------

bool EspNowBroadcastDriver::start_broadcast(uint8_t channel) {
    if (active_) return true;
    auto* radio = hal::HAL::esp_now();
    if (!radio) return false;
    source_id_  = derive_source_id();
    seq_num_    = 1;
    last_tx_ms_ = 0;
    active_ = radio->begin(channel);
#ifdef ARDUINO
    if (!active_) {
        Serial.println("[espnow] broadcaster begin() failed");
    } else {
        Serial.printf("[espnow] broadcaster up: ch=%u src_id=%u\n",
                      (unsigned)channel, (unsigned)source_id_);
    }
#endif
    return active_;
}

void EspNowBroadcastDriver::stop_broadcast() {
    if (!active_) return;
    if (auto* radio = hal::HAL::esp_now()) radio->end();
    active_ = false;
}

bool EspNowBroadcastDriver::send_music_event(transport::espnow::MusicEventType event_type) {
    if (!active_) return false;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    MusicEventPayload p{ event_type };
    uint8_t buf[kHeaderSize + kMusicEventPayloadLen];
    const size_t n = encode_music_event(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "MUSIC");
    return true;
}

// -----------------------------------------------------------------------------
// Internal helpers (verbatim from EspNowBroadcaster)
// -----------------------------------------------------------------------------

uint8_t EspNowBroadcastDriver::derive_source_id() {
#ifdef ARDUINO
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    uint8_t id = mac[5];
    if (id == 0 || id == 0xFF) id = (mac[4] != 0 && mac[4] != 0xFF) ? mac[4] : 1;
    return id;
#else
    return 1;
#endif
}

uint8_t EspNowBroadcastDriver::next_seq() {
    const uint8_t s = seq_num_;
    seq_num_ = (seq_num_ == 255) ? 1 : (seq_num_ + 1);
    return s;
}

void EspNowBroadcastDriver::send_frame_bytes(const uint8_t* buf, size_t n, const char* label) {
    if (!active_ || n == 0) return;
    auto* radio = hal::HAL::esp_now();
    if (!radio) return;
    const bool ok = radio->send_broadcast(buf, n);
    if (ok) last_tx_ms_ = now_ms();
#ifdef ARDUINO
    Serial.printf("[espnow TX %s%s] ", label, ok ? "" : " FAIL");
    for (size_t i = 0; i < n; ++i) Serial.printf("%02X ", buf[i]);
    Serial.println();
#else
    (void)ok; (void)label;
#endif

    // Schedule the redundant retransmits per spec §4.3. New frame
    // replaces any pending retransmit queue - if a beat lands while
    // a heartbeat is still mid-burst, we'd rather get the beat out
    // than complete the heartbeat's redundancy.
    if (n <= kRetransmitBufSize) {
        std::memcpy(retransmit_buf_, buf, n);
        retransmit_len_        = n;
        retransmits_remaining_ = kRedundantSends - 1;
        next_retransmit_ms_    = now_ms() + redundant_gap_ms();
    } else {
        retransmits_remaining_ = 0;
    }
}

void EspNowBroadcastDriver::pump_retransmits() {
    if (!active_ || retransmits_remaining_ == 0) return;
    const uint32_t now = now_ms();
    if (now < next_retransmit_ms_) return;

    auto* radio = hal::HAL::esp_now();
    if (!radio) {
        retransmits_remaining_ = 0;
        return;
    }
    radio->send_broadcast(retransmit_buf_, retransmit_len_);
    last_tx_ms_ = now;
    retransmits_remaining_--;
    if (retransmits_remaining_ > 0) {
        next_retransmit_ms_ = now + redundant_gap_ms();
    }
}

uint32_t EspNowBroadcastDriver::redundant_gap_ms() {
    // Pseudo-random jitter in [kRedundantGapMinMs, kRedundantGapMaxMs].
    // std::rand() is good enough - we want spread, not cryptographic
    // unpredictability.
    const uint32_t span = kRedundantGapMaxMs - kRedundantGapMinMs + 1;
    return kRedundantGapMinMs + (std::rand() % span);
}

void EspNowBroadcastDriver::send_heartbeat() {
    if (!active_) return;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    uint8_t buf[kHeaderSize + kHeartbeatPayloadLen];
    const size_t n = encode_heartbeat(buf, sizeof(buf), h);
    send_frame_bytes(buf, n, "HBEAT");
}

bool EspNowBroadcastDriver::maybe_send_heartbeat() {
    if (!active_) return false;
    const uint32_t now = now_ms();
    const uint32_t gap = now - last_tx_ms_;
    if (gap < kHeartbeatPeriodMs) return false;
#ifdef ARDUINO
    Serial.printf("[HBEAT] firing after %lu ms gap since last TX\n",
                  static_cast<unsigned long>(gap));
#endif
    send_heartbeat();
    return true;
}

}  // namespace dal
}  // namespace nocturnation
