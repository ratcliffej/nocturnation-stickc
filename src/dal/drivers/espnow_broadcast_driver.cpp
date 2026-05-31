// EspNowBroadcastDriver implementation. Lifted from the per-mode
// EspNowBroadcaster helper (src/modes/espnow_broadcaster.{h,cpp}) in
// Epic 4.6 Block 2. Wire output, sequence numbering, source_id derivation,
// retransmit jitter and heartbeat skip-if-recent are preserved exactly.

#include "espnow_broadcast_driver.h"

#include "hal/hal.h"
#include "modes/persistence.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <WiFi.h>
#include <esp_random.h>
#endif

namespace nocturnation {
namespace dal {

namespace {
EspNowBroadcastDriver s_instance;

#ifndef ARDUINO
// Native test seam state. Tests reach this via the test_seam:: free
// functions at the bottom of this TU.
uint32_t s_native_now_ms = 0;

constexpr size_t kPickQueueCap = 8;
uint8_t  s_pick_queue[kPickQueueCap] = {};
size_t   s_pick_queue_count          = 0;
#endif

// now_ms() shim. In ARDUINO builds this is the real ::millis(); in
// native builds it reads s_native_now_ms which tests drive explicitly
// (defaults to 0 so existing tests that ignore time still see 0 like
// before B4).
inline uint32_t now_ms() {
#ifdef ARDUINO
    return ::millis();
#else
    return s_native_now_ms;
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
    // Listen-before-broadcast (ch 11) gate: while in Listening we hold
    // TX off and watch for collisions; listen_tick() settles us into
    // Active once the listen window elapses (with or without re-roll).
    if (startup_state_ == StartupState::Listening) {
        listen_tick();
        return;
    }
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
    LightPulsePayload p{};
    p.target_class = target_class;
    p.target_group = target_group;
    p.r = ev.r; p.g = ev.g; p.b = ev.b;
    p.attack  = static_cast<uint8_t>(ev.attack);
    p.sustain = static_cast<uint8_t>(ev.sustain);
    p.release = static_cast<uint8_t>(ev.release);
    p.chance  = static_cast<uint8_t>(ev.chance);
    uint8_t buf[kHeaderSize + kLightPulsePayloadLen];
    const size_t n = encode_light_pulse(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "LIGHT");
    return true;
}

// -----------------------------------------------------------------------------
// Driver-specific lifecycle / protocol entry points
// -----------------------------------------------------------------------------

bool EspNowBroadcastDriver::start_broadcast(uint8_t channel) {
    if (active_ || startup_state_ != StartupState::Idle) return active_;
    auto* radio = hal::HAL::esp_now();
    if (!radio) return false;
    seq_num_    = 1;
    last_tx_ms_ = 0;

    if (channel == 11) {
        // Channel 11 (Performance mode): pick a Performance-range id,
        // install a listen-window recv callback, bring the radio up
        // RX-capable, and leave active_ = false so loop_tick() holds
        // TX off until listen_tick() settles. See spec §3.4 and the
        // B4a design notes in the Epic working copy.
        listen_candidate_          = pick_performance_id_random();
        listen_collision_heard_    = false;
        listen_attempts_remaining_ = kListenMaxAttempts;
        radio->set_recv_callback([this](const hal::ESPNowMessage& m) {
            this->on_listen_recv(m);
        });
        if (!radio->begin(channel)) {
            radio->set_recv_callback(nullptr);
#ifdef ARDUINO
            Serial.println("[espnow] broadcaster begin(11) failed");
#endif
            return false;
        }
        listen_started_ms_ = now_ms();
        startup_state_     = StartupState::Listening;
#ifdef ARDUINO
        Serial.printf("[espnow] broadcaster listening: ch=11 candidate=0x%02X\n",
                      (unsigned)listen_candidate_);
#endif
        return true;
    }

    // Channels 1 and 6: original path. source_id derives synchronously
    // (community range on ch 1 via persistence, MAC-derive on ch 6).
    source_id_ = derive_source_id(channel);
    active_    = radio->begin(channel);
    startup_state_ = active_ ? StartupState::Active : StartupState::Idle;
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
    if (startup_state_ == StartupState::Idle && !active_) return;
    if (auto* radio = hal::HAL::esp_now()) {
        // Tear down the listen callback whether or not we were listening;
        // safe no-op if we never installed one.
        radio->set_recv_callback(nullptr);
        radio->end();
    }
    active_                    = false;
    startup_state_             = StartupState::Idle;
    listen_collision_heard_    = false;
    listen_attempts_remaining_ = 0;
}

// -----------------------------------------------------------------------------
// Internal helpers (verbatim from EspNowBroadcaster)
// -----------------------------------------------------------------------------

uint8_t EspNowBroadcastDriver::derive_source_id(uint8_t channel) {
    // Channel 1: community range (Epic 5.5 B3). The value was rolled at
    // first boot inside migrate_legacy_nvs_keys and persisted to NVS,
    // so every subsequent boot loads the same stable per-device ID. A
    // returning Lume locks back to the same Director ID across power-
    // cycles, which is the contract channel 1 is built on.
    if (channel == 1) {
        return modes::persistence::load_director_source_id();
    }

    // Channels 6 and 11: legacy MAC-derived behaviour until B4 lands
    // the Performance-range random-per-boot + listen-before-broadcast
    // path for channel 11. Channel 6 stays operator-discretionary per
    // spec §3.4 and is left on the MAC derivation indefinitely.
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
    // Spec v0.29 §4.3 HEARTBEAT payload: tick (Director clock) + date
    // fields (zeroed when no wall clock is configured; date sync is a
    // Tier 3 concern not implemented yet).
    HeartbeatPayload p{};
    p.tick               = now_ms();
    p.days_since_2026    = 0;
    p.centiseconds_today = 0;
    uint8_t buf[kHeaderSize + kHeartbeatPayloadLen];
    const size_t n = encode_heartbeat(buf, sizeof(buf), h, p);
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

// -----------------------------------------------------------------------------
// Channel-11 listen-before-broadcast (Epic 5.5 B4)
// -----------------------------------------------------------------------------

void EspNowBroadcastDriver::listen_tick() {
    // Called only while startup_state_ == Listening; loop_tick() gates this.
    const uint32_t now = now_ms();
    if (now - listen_started_ms_ < kListenWindowMs) return;

    if (listen_collision_heard_) {
        // listen_attempts_remaining_ was set to kListenMaxAttempts at
        // start_broadcast; decrement each time we conclude a collided
        // window. When it reaches zero we proceed with the most-recent
        // candidate per spec §3.4 ("after three consecutive collisions
        // the Director MAY proceed and SHOULD log a warning").
        --listen_attempts_remaining_;
        if (listen_attempts_remaining_ == 0) {
            log_listen_collision_warning();
            // fall through to settle
        } else {
            listen_candidate_       = pick_performance_id_random();
            listen_collision_heard_ = false;
            listen_started_ms_      = now;
#ifdef ARDUINO
            Serial.printf("[espnow] listen collision; re-rolling to 0x%02X "
                          "(attempts left=%u)\n",
                          (unsigned)listen_candidate_,
                          (unsigned)listen_attempts_remaining_);
#endif
            return;
        }
    }

    // Settle: window elapsed without (further) collision, or attempts exhausted.
    source_id_     = listen_candidate_;
    if (auto* radio = hal::HAL::esp_now()) {
        radio->set_recv_callback(nullptr);
    }
    active_        = true;
    startup_state_ = StartupState::Active;
#ifdef ARDUINO
    Serial.printf("[espnow] broadcaster settled: ch=11 src_id=0x%02X\n",
                  (unsigned)source_id_);
#endif
}

void EspNowBroadcastDriver::on_listen_recv(const hal::ESPNowMessage& m) {
    // Only HEARTBEATs matching our candidate id count as a collision.
    // Everything else - other message types, frames addressed to other
    // candidate ids, or malformed frames - is silently ignored. We MUST
    // NOT route inbound frames to renderers from here; Director Mode
    // has no inbound consumer in steady state, so the recv callback's
    // sole job during the listen window is collision detection.
    using namespace transport::espnow;
    Header hdr{};
    if (decode_header(m.data, m.len, hdr) != DecodeResult::Ok) return;
    if (hdr.message_type != MessageType::Heartbeat) return;
    if (hdr.source_id == listen_candidate_) {
        listen_collision_heard_ = true;
    }
}

void EspNowBroadcastDriver::log_listen_collision_warning() const {
#ifdef ARDUINO
    Serial.printf("[espnow] listen collision on all %u attempts; "
                  "proceeding with src_id=0x%02X (operationally extremely rare)\n",
                  (unsigned)kListenMaxAttempts,
                  (unsigned)listen_candidate_);
#endif
}

size_t EspNowBroadcastDriver::format_status_label(StartupState state,
                                                   uint8_t source_id_value,
                                                   uint8_t listen_candidate_value,
                                                   char* buf,
                                                   size_t buflen) {
    if (buf == nullptr || buflen == 0) return 0;
    buf[0] = '\0';
    if (state == StartupState::Idle) return 0;

    using namespace transport::espnow;
    const uint8_t id = (state == StartupState::Listening)
                           ? listen_candidate_value : source_id_value;

    char prefix;
    if (is_community_range(id))        prefix = 'C';
    else if (is_performance_range(id)) prefix = 'P';
    else                               prefix = '?';   // defensive (0xFF / corrupt)

    const char* suffix = (state == StartupState::Listening) ? "?" : "";
    const int n = std::snprintf(buf, buflen, "%c:%02X%s",
                                prefix, (unsigned)id, suffix);
    if (n <= 0) {
        buf[0] = '\0';
        return 0;
    }
    if (static_cast<size_t>(n) >= buflen) {
        // snprintf truncated. Surface as "no label" rather than a
        // partial that might mislead the operator.
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

uint8_t EspNowBroadcastDriver::pick_performance_id_random() {
#ifdef ARDUINO
    // Performance range: 0x40..0xFE inclusive = 191 slots.
    return static_cast<uint8_t>(0x40 + (esp_random() % 191));
#else
    if (s_pick_queue_count == 0) {
        // Deterministic floor for tests that forget to queue.
        return 0x40;
    }
    const uint8_t id = s_pick_queue[0];
    for (size_t i = 1; i < s_pick_queue_count; ++i) {
        s_pick_queue[i - 1] = s_pick_queue[i];
    }
    --s_pick_queue_count;
    return id;
#endif
}

#ifndef ARDUINO
// -----------------------------------------------------------------------------
// Native test seam
// -----------------------------------------------------------------------------

void EspNowBroadcastDriver::test_enter_listening(uint8_t candidate,
                                                  uint32_t started_ms) {
    // Force the driver into Listening without going through start_broadcast
    // (which needs a live ESPNow HAL we don't stub in native tests).
    listen_candidate_          = candidate;
    listen_collision_heard_    = false;
    listen_attempts_remaining_ = kListenMaxAttempts;
    listen_started_ms_         = started_ms;
    active_                    = false;
    startup_state_             = StartupState::Listening;
}

void EspNowBroadcastDriver::test_inject_listen_heartbeat(uint8_t source_id) {
    // Build a minimal valid HEARTBEAT frame and route it into the listen
    // callback as if it had come from the radio. The frame buffer lives
    // on the stack here; on_listen_recv() consumes synchronously.
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id;
    h.sequence_number = 1;
    h.hop_count       = 0;
    HeartbeatPayload p{};
    p.tick               = 0;
    p.days_since_2026    = 0;
    p.centiseconds_today = 0;
    uint8_t buf[kHeaderSize + kHeartbeatPayloadLen];
    const size_t n = encode_heartbeat(buf, sizeof(buf), h, p);
    if (n == 0) return;
    hal::ESPNowMessage msg{};
    msg.timestamp_ms = 0;
    msg.data         = buf;
    msg.len          = n;
    msg.rssi         = -50;
    on_listen_recv(msg);
}

namespace test_seam {
void set_now_ms(uint32_t ms) {
    s_native_now_ms = ms;
}

void queue_next_performance_pick(uint8_t id_in_performance_range) {
    if (s_pick_queue_count < kPickQueueCap) {
        s_pick_queue[s_pick_queue_count++] = id_in_performance_range;
    }
}

void clear_native_driver_state() {
    s_native_now_ms     = 0;
    s_pick_queue_count  = 0;
    for (size_t i = 0; i < kPickQueueCap; ++i) s_pick_queue[i] = 0;
}
}  // namespace test_seam
#endif

}  // namespace dal
}  // namespace nocturnation
