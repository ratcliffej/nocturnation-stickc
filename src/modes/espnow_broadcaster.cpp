// EspNowBroadcaster implementation.

#include "espnow_broadcaster.h"

#include "hal/hal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using nocturnation::dal::RgbPulseEvent;

bool EspNowBroadcaster::begin(uint8_t channel) {
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

void EspNowBroadcaster::end() {
    if (!active_) return;
    if (auto* radio = hal::HAL::esp_now()) radio->end();
    active_ = false;
}

uint8_t EspNowBroadcaster::derive_source_id() {
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

uint8_t EspNowBroadcaster::next_seq() {
    const uint8_t s = seq_num_;
    seq_num_ = (seq_num_ == 255) ? 1 : (seq_num_ + 1);
    return s;
}

void EspNowBroadcaster::send_frame_bytes(const uint8_t* buf, size_t n, const char* label) {
    if (!active_ || n == 0) return;
    auto* radio = hal::HAL::esp_now();
    if (!radio) return;
    const bool ok = radio->send_broadcast(buf, n);
    if (ok) last_tx_ms_ = millis();
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
        next_retransmit_ms_    = millis() + redundant_gap_ms();
    } else {
        retransmits_remaining_ = 0;
    }
}

void EspNowBroadcaster::pump_retransmits() {
    if (!active_ || retransmits_remaining_ == 0) return;
    const uint32_t now = millis();
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

uint32_t EspNowBroadcaster::redundant_gap_ms() {
    // Pseudo-random jitter in [kRedundantGapMinMs, kRedundantGapMaxMs].
    // std::rand() is good enough - we want spread, not cryptographic
    // unpredictability.
    const uint32_t span = kRedundantGapMaxMs - kRedundantGapMinMs + 1;
    return kRedundantGapMinMs + (std::rand() % span);
}

void EspNowBroadcaster::send_beat(float strength_rms, float bpm) {
    if (!active_) return;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    BeatDetectedPayload p{};
    const float scaled = strength_rms / 20.0f;
    p.strength = (scaled < 0.0f)   ? 0
               : (scaled > 255.0f) ? 255
                                   : static_cast<uint8_t>(scaled);
    const float bpm_x10 = bpm * 10.0f;
    p.bpm_x10 = (bpm_x10 < 0.0f)     ? 0
              : (bpm_x10 > 65535.0f) ? 65535
                                     : static_cast<uint16_t>(bpm_x10);
    uint8_t buf[kHeaderSize + kBeatDetectedPayloadLen];
    const size_t n = encode_beat_detected(buf, sizeof(buf), h, p);
    send_frame_bytes(buf, n, "BEAT");
}

void EspNowBroadcaster::send_heartbeat() {
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

void EspNowBroadcaster::send_music_event(transport::espnow::MusicEventType event_type) {
    if (!active_) return;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    MusicEventPayload p{ event_type };
    uint8_t buf[kHeaderSize + kMusicEventPayloadLen];
    const size_t n = encode_music_event(buf, sizeof(buf), h, p);
    send_frame_bytes(buf, n, "MUSIC");
}

void EspNowBroadcaster::send_light_command(uint8_t target_group,
                                           uint8_t r, uint8_t g, uint8_t b,
                                           effects::PulseEnvelope env,
                                           pixmob::Chance chance) {
    if (!active_) return;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    LightCommandPayload p{};
    p.target_group = target_group;
    p.r = r; p.g = g; p.b = b;
    p.attack  = static_cast<uint8_t>(env.attack);
    p.sustain = static_cast<uint8_t>(env.sustain);
    p.release = static_cast<uint8_t>(env.release);
    p.chance  = static_cast<uint8_t>(chance);
    uint8_t buf[kHeaderSize + kLightCommandPayloadLen];
    const size_t n = encode_light_command(buf, sizeof(buf), h, p);
    send_frame_bytes(buf, n, "LIGHT");
}

void EspNowBroadcaster::send_light_command(uint8_t target_group, const RgbPulseEvent& ev) {
    send_light_command(target_group, ev.r, ev.g, ev.b,
                       effects::PulseEnvelope{ev.attack, ev.sustain, ev.release},
                       ev.chance);
}

bool EspNowBroadcaster::maybe_send_heartbeat() {
    if (!active_) return false;
    const uint32_t now = millis();
    const uint32_t gap = now - last_tx_ms_;
    if (gap < kHeartbeatPeriodMs) return false;
#ifdef ARDUINO
    Serial.printf("[HBEAT] firing after %lu ms gap since last TX\n",
                  static_cast<unsigned long>(gap));
#endif
    send_heartbeat();
    return true;
}

}  // namespace modes
}  // namespace nocturnation
