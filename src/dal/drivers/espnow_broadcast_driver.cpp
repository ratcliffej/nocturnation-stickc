// EspNowBroadcastDriver implementation.

#include "espnow_broadcast_driver.h"

#include "hal/hal.h"
#include "modes/persistence.h"
#include "repeater_census.h"

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
// Native test seam state; driven via test_seam:: below.
uint32_t s_native_now_ms = 0;

constexpr size_t kPickQueueCap = 8;
uint8_t  s_pick_queue[kPickQueueCap] = {};
size_t   s_pick_queue_count          = 0;
#endif

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
    // Registration gate only - modes call start_broadcast() to take the air.
    return hal::HAL::esp_now() != nullptr;
}

void EspNowBroadcastDriver::loop_tick() {
    if (startup_state_ == StartupState::Listening) {
        listen_tick();
        return;
    }
    if (!active_) return;
    pump_retransmits();
    maybe_send_heartbeat();
}

bool EspNowBroadcastDriver::send(uint8_t group_id, const RgbPulseEvent& ev) {
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
    // Epic 18 (v4) LED addressing. Default 0/0/0 (LedMode::All) matches
    // v3-era behaviour; test-mode / show-side callers set explicit
    // targeted modes.
    p.led_mode      = ev.led_mode;
    p.led_modifier1 = ev.led_modifier1;
    p.led_modifier2 = ev.led_modifier2;
    uint8_t buf[kHeaderSize + kLightPulsePayloadLen];
    const size_t n = encode_light_pulse(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "LIGHT");
    return true;
}

// -----------------------------------------------------------------------------
// WASH-family senders
// -----------------------------------------------------------------------------

bool EspNowBroadcastDriver::send_wash(uint8_t target_class,
                                       uint8_t target_group,
                                       const LightWashEvent& ev) {
    if (!active_) return false;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    LightWashPayload p{};
    p.target_class   = target_class;
    p.target_group   = target_group;
    p.r1 = ev.r1; p.g1 = ev.g1; p.b1 = ev.b1;
    p.r2 = ev.r2; p.g2 = ev.g2; p.b2 = ev.b2;
    p.attack         = ev.attack;
    p.release        = ev.release;
    p.intensity      = ev.intensity;
    p.cycle_ms       = ev.cycle_ms;
    p.ttl_seconds    = ev.ttl_seconds;
    p.pulse_response = ev.pulse_response;
    p.led_mode       = ev.led_mode;         // Epic 18 (v4)
    p.led_modifier1  = ev.led_modifier1;    // Epic 18 (v4)
    p.led_modifier2  = ev.led_modifier2;    // Epic 18 (v4)
    uint8_t buf[kHeaderSize + kLightWashPayloadLen];
    const size_t n = encode_light_wash(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "WASH");
    return true;
}

bool EspNowBroadcastDriver::send_wash_end(uint8_t target_class,
                                          uint8_t target_group,
                                          uint8_t release_time) {
    if (!active_) return false;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    LightWashEndPayload p{};
    p.target_class = target_class;
    p.target_group = target_group;
    p.release_time = release_time;
    uint8_t buf[kHeaderSize + kLightWashEndPayloadLen];
    const size_t n = encode_light_wash_end(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "WEND");
    return true;
}

bool EspNowBroadcastDriver::send_wash_pulse(uint8_t target_class,
                                             uint8_t target_group,
                                             const RgbPulseEvent& ev) {
    if (!active_) return false;
    using namespace transport::espnow;
    Header h{};
    h.source_id       = source_id_;
    h.sequence_number = next_seq();
    h.hop_count       = 0;
    LightWashPulsePayload p{};
    p.target_class = target_class;
    p.target_group = target_group;
    p.r = ev.r; p.g = ev.g; p.b = ev.b;
    p.attack  = static_cast<uint8_t>(ev.attack);
    p.sustain = static_cast<uint8_t>(ev.sustain);
    p.release = static_cast<uint8_t>(ev.release);
    p.chance  = static_cast<uint8_t>(ev.chance);
    p.led_mode      = ev.led_mode;         // Epic 18 (v4)
    p.led_modifier1 = ev.led_modifier1;    // Epic 18 (v4)
    p.led_modifier2 = ev.led_modifier2;    // Epic 18 (v4)
    uint8_t buf[kHeaderSize + kLightWashPulsePayloadLen];
    const size_t n = encode_light_wash_pulse(buf, sizeof(buf), h, p);
    if (n == 0) return false;
    send_frame_bytes(buf, n, "WPUL");
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
        // Performance mode: load the persisted candidate, hold TX off,
        // let listen_tick() settle us into Active per spec §3.4. A
        // re-roll on collision is persisted so subsequent boots align
        // with whatever id is actually on air.
        listen_candidate_          = modes::persistence::load_director_perf_source_id();
        listen_collision_heard_    = false;
        listen_attempts_remaining_ = kListenMaxAttempts;
        radio->set_recv_callback([this](const hal::ESPNowMessage& m) {
            this->on_recv(m);
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

    source_id_ = derive_source_id(channel);
    active_    = radio->begin(channel);
    startup_state_ = active_ ? StartupState::Active : StartupState::Idle;
    // Install recv callback on 1/6 too so the repeater census is tallied.
    if (active_) {
        radio->set_recv_callback([this](const hal::ESPNowMessage& m) {
            this->on_recv(m);
        });
    }
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
        radio->set_recv_callback(nullptr);
        radio->end();
    }
    active_                    = false;
    startup_state_             = StartupState::Idle;
    listen_collision_heard_    = false;
    listen_attempts_remaining_ = 0;
}

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

uint8_t EspNowBroadcastDriver::derive_source_id(uint8_t channel) {
    // Channel 1: stable community-range id persisted at first boot so
    // returning Lumes relock to the same Director across power cycles.
    if (channel == 1) {
        return modes::persistence::load_director_source_id();
    }

    // Channel 6 stays on MAC derivation (operator-discretionary per §3.4).
    // Channel 11 is handled by the listen-before-broadcast path in
    // start_broadcast() and never reaches here.
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
    // Suppress the diagnostic line on the AtomS3-PoE Director (Serial is
    // its config console).
#if defined(ARDUINO) && !defined(NOCT_DMX_ETHERNET)
    Serial.printf("[espnow TX %s%s len=%u]\n", label, ok ? "" : " FAIL", (unsigned)n);
#else
    (void)ok; (void)label;
#endif

    // A fresh frame replaces any pending retransmit queue: getting a
    // new beat on air beats finishing an old frame's redundancy.
    if (n <= kRetransmitBufSize) {
        std::memcpy(retransmit_buf_, buf, n);
        retransmit_len_        = n;
        retransmits_remaining_ = kRedundantSends - 1;
        next_retransmit_ms_    = now_ms() + redundant_gap_ms();
    } else {
        retransmits_remaining_ = 0;
    }
}

void EspNowBroadcastDriver::send_passthrough(const uint8_t* buf, size_t n) {
    // Validate magic + version before mutating - never let a misbehaving
    // upstream sender put bad bytes on air under this Director's identity.
    if (n < transport::espnow::kHeaderSize) return;
    if (buf[0] != transport::espnow::kMagic0
        || buf[1] != transport::espnow::kMagic1) return;
    if (buf[2] != transport::espnow::kProtocolVersion) return;
    if (!active_) return;

    // Re-stamp source_id and sequence_number when upstream used the
    // broadcast id (0xFFFF): joins this Director's single monotonic seq
    // stream so receivers dedup correctly. See docs/stickc-history.md
    // for why the seq re-stamp is load-bearing. A frame with its own
    // source_id keeps its own seq (preserved-identity case). v3 layout:
    //   offset 3-4: source_id LE u16
    //   offset 5:   sequence_number
    uint8_t patched[transport::espnow::kMaxFrameSize];
    if (n > sizeof(patched)) return;
    std::memcpy(patched, buf, n);
    const uint16_t inbound_src =
        static_cast<uint16_t>(patched[3]) |
        (static_cast<uint16_t>(patched[4]) << 8);
    if (inbound_src == transport::espnow::kBroadcastSourceId) {
        const uint16_t src = source_id_;
        patched[3] = static_cast<uint8_t>(src        & 0xFF);   // src LSB
        patched[4] = static_cast<uint8_t>((src >> 8) & 0xFF);   // src MSB
        patched[5] = next_seq();                                // seq (v3 offset)
    }
    send_frame_bytes(patched, n, "PASS");
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
    // Date fields zeroed until wall-clock sync (Tier 3) lands.
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
    // Gate on last_hb_ms_ (not last_tx_ms_) so continuous traffic can't
    // suppress the §4.3 tick anchor. See docs/stickc-history.md.
    const uint32_t gap = now - last_hb_ms_;
    if (gap < kHeartbeatPeriodMs) return false;
    // Suppressed on the AtomS3-PoE Director (Serial is its config console).
#if defined(ARDUINO) && !defined(NOCT_DMX_ETHERNET)
    Serial.printf("[HBEAT] firing after %lu ms gap since last HB\n",
                  static_cast<unsigned long>(gap));
#endif
    send_heartbeat();
    last_hb_ms_ = now;
    return true;
}

// -----------------------------------------------------------------------------
// Channel-11 listen-before-broadcast
// -----------------------------------------------------------------------------

void EspNowBroadcastDriver::listen_tick() {
    const uint32_t now = now_ms();
    if (now - listen_started_ms_ < kListenWindowMs) return;

    if (listen_collision_heard_) {
        --listen_attempts_remaining_;
        if (listen_attempts_remaining_ == 0) {
            log_listen_collision_warning();
            // fall through to settle per spec §3.4
        } else {
            listen_candidate_       = pick_performance_id_random();
            listen_collision_heard_ = false;
            listen_started_ms_      = now;
            // Persist the re-rolled id: whatever the radio environment
            // forces us onto is the new stable identity for this device.
            modes::persistence::save_director_perf_source_id(listen_candidate_);
#ifdef ARDUINO
            Serial.printf("[espnow] listen collision; re-rolling to 0x%02X "
                          "(attempts left=%u)\n",
                          (unsigned)listen_candidate_,
                          (unsigned)listen_attempts_remaining_);
#endif
            return;
        }
    }

    // Window elapsed without further collision (or attempts exhausted).
    // Recv callback stays installed to keep the repeater census tallying.
    source_id_     = listen_candidate_;
    active_        = true;
    startup_state_ = StartupState::Active;
#ifdef ARDUINO
    Serial.printf("[espnow] broadcaster settled: ch=11 src_id=0x%02X\n",
                  (unsigned)source_id_);
#endif
}

void EspNowBroadcastDriver::on_listen_recv(const hal::ESPNowMessage& m) {
    // Only HEARTBEATs matching our candidate id count as a collision.
    // Must never route inbound frames to renderers from here: the
    // recv callback's sole job during the listen window is collision
    // detection.
    using namespace transport::espnow;
    Header hdr{};
    if (decode_header(m.data, m.len, hdr) != DecodeResult::Ok) return;
    if (hdr.message_type != MessageType::Heartbeat) return;
    if (hdr.source_id == listen_candidate_) {
        listen_collision_heard_ = true;
    }
}

void EspNowBroadcastDriver::on_recv(const hal::ESPNowMessage& m) {
    if (startup_state_ == StartupState::Listening) {
        on_listen_recv(m);
    }
    feed_census(m);
}

void EspNowBroadcastDriver::feed_census(const hal::ESPNowMessage& m) {
    using namespace transport::espnow;
    Header hdr{};
    if (decode_header(m.data, m.len, hdr) != DecodeResult::Ok) return;
    if (hdr.message_type != MessageType::RepeaterHeartbeat) return;
    // Direct census only (hop 0) so a relayed beacon doesn't double-count.
    if (hdr.hop_count != 0) return;
    RepeaterHeartbeatPayload p{};
    if (decode_repeater_heartbeat(hdr, m.data + kHeaderSize,
                                  hdr.payload_len, p) != DecodeResult::Ok) {
        return;
    }
    repeater_census_instance().note(p.uid, p.channel, p.relayed_count,
                                    p.uptime_s, now_ms());
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
        // Truncated: return "no label" rather than a partial that
        // might mislead the operator.
        buf[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(n);
}

uint8_t EspNowBroadcastDriver::pick_performance_id_random() {
#ifdef ARDUINO
    // Performance range 0x40..0xFE = 191 slots.
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
    // Bypass start_broadcast (which needs a live ESPNow HAL).
    listen_candidate_          = candidate;
    listen_collision_heard_    = false;
    listen_attempts_remaining_ = kListenMaxAttempts;
    listen_started_ms_         = started_ms;
    active_                    = false;
    startup_state_             = StartupState::Listening;
}

void EspNowBroadcastDriver::test_inject_listen_heartbeat(uint8_t source_id) {
    // Build a minimal valid HEARTBEAT and route it into on_listen_recv,
    // which consumes synchronously so the stack buffer is safe.
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
