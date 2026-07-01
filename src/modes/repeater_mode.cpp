// RepeaterMode implementation - headless ESP-NOW repeater.

#include "repeater_mode.h"

#include "persistence.h"
#include "transport/espnow/frame.h"

#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>       // millis(), neopixelWrite()
#include <M5Unified.h>     // M5.BtnA (M5.update() runs in HAL::loop_tick)
#include <WiFi.h>          // WiFi.macAddress() for the census uid
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

namespace te = ::nocturnation::transport::espnow;

namespace {

// Channels probed in auto mode, in order. Matches the fleet's valid set
// (1 = hobby, 6 = advanced, 11 = show) per architecture spec section 4.5.
constexpr uint8_t kScanChannels[3] = {1, 6, 11};

// Dim base colours per channel for the status LED. Kept low - the AtomS3
// onboard LED is bright and this runs continuously. Brightened for the
// change-confirm flash.
void channel_colour(uint8_t ch, uint8_t& r, uint8_t& g, uint8_t& b) {
    switch (ch) {
        case 1:  r = 0;  g = 0;  b = 24; break;   // blue
        case 6:  r = 0;  g = 24; b = 0;  break;   // green
        case 11: r = 24; g = 0;  b = 24; break;   // magenta
        default: r = 16; g = 16; b = 16; break;   // white-ish = auto/unknown
    }
}

}  // namespace

void RepeaterMode::enter() {
    boot_ms_        = millis();
    last_census_ms_ = boot_ms_;
    pending_repeat_ = false;
    dedup_head_     = 0;
    relayed_count_  = 0;
    last_relay_ms_  = 0;
    last_rx_ms_     = 0;

    channel_setting_ = persistence::load_repeater_channel();
    if (channel_setting_ == 0) {
        // Auto: begin probing on the first channel; on_recv locks us to
        // whichever carries traffic.
        locked_                = false;
        scan_idx_              = 0;
        scan_probe_started_ms_ = boot_ms_;
        start_radio(kScanChannels[0]);
    } else {
        locked_ = true;
        start_radio(channel_setting_);
    }

    if (auto* radio = hal::HAL::esp_now()) {
        radio->set_recv_callback(
            [this](const hal::ESPNowMessage& m) { this->on_recv(m); });
    }

#ifdef ARDUINO
    // Stable 3-byte id from the STA MAC (the radio is up by now). The
    // Director dedups the census on this; the census header carries the
    // anonymous broadcast source id (see emit_census).
    uint8_t mac[6] = {0};
    WiFi.macAddress(mac);
    uid_[0] = mac[3];
    uid_[1] = mac[4];
    uid_[2] = mac[5];
#endif
}

void RepeaterMode::exit() {
    if (auto* radio = hal::HAL::esp_now()) {
        radio->set_recv_callback(nullptr);
    }
    pending_repeat_ = false;
#ifdef ARDUINO
    neopixelWrite(35, 0, 0, 0);
#endif
}

void RepeaterMode::start_radio(uint8_t channel) {
    auto* radio = hal::HAL::esp_now();
    if (radio == nullptr) {
        radio_up_ = false;
        return;
    }
    // begin() is idempotent and brings up ESP-NOW Long Range on first
    // call; set_channel() then guarantees we're on the requested channel
    // whether or not the radio was already running.
    radio->begin(channel);
    radio->set_channel(channel);
    active_channel_ = channel;
    radio_up_       = true;
}

void RepeaterMode::on_recv(const hal::ESPNowMessage& m) {
    // Runs on the WiFi task: validate + dedup + stage one relay only.
    // No TX and no blocking work here (same rule as Lume repeat).
    if (m.data == nullptr || m.len < te::kHeaderSize) return;

    te::Header hdr{};
    if (te::decode_header(m.data, m.len, hdr) != te::DecodeResult::Ok) return;

    // Census is point-to-Director telemetry: never relayed (repeaters
    // would echo each other's census) and never counted as channel
    // traffic - two idle repeaters refreshing each other's liveness at
    // 1 Hz would otherwise stay locked to a dead channel forever.
    if (hdr.message_type == te::MessageType::RepeaterHeartbeat) return;

    // Any other valid fleet frame proves this channel carries traffic -
    // the auto-scan lock signal, and the liveness clock for re-scan.
    last_rx_ms_ = millis();
    locked_     = true;

    if (seen_recently(hdr.source_id, hdr.sequence_number)) return;
    mark_seen(hdr.source_id, hdr.sequence_number);

    if (hdr.hop_count < kMaxHopCount && m.len <= kRepeatBufSize) {
        std::memcpy(pending_repeat_buf_, m.data, m.len);
        // hop_count is at on-wire byte offset 5.
        pending_repeat_buf_[5] = static_cast<uint8_t>(hdr.hop_count + 1);
        pending_repeat_len_    = m.len;
        pending_repeat_        = true;
    }
}

void RepeaterMode::loop_tick() {
    const uint32_t now = millis();

    // Flush a staged relay first, for lowest added latency.
    if (pending_repeat_) {
        pending_repeat_ = false;
        if (auto* radio = hal::HAL::esp_now()) {
            if (radio->send_broadcast(pending_repeat_buf_,
                                      pending_repeat_len_)) {
                ++relayed_count_;
                last_relay_ms_ = now;
            }
        }
    }

    poll_button(now);
    service_scan(now);
    emit_census(now);
    update_led(now);
}

void RepeaterMode::poll_button(uint32_t now) {
#ifdef ARDUINO
    // HAL::loop_tick() already ran M5.update() this iteration, so BtnA
    // edge state is current. Detect a double-click from two clicks inside
    // kDoubleClickMs and cycle the channel setting.
    if (M5.BtnA.wasClicked()) {
        if (last_click_ms_ != 0 && (now - last_click_ms_) <= kDoubleClickMs) {
            last_click_ms_ = 0;
            uint8_t next;
            switch (channel_setting_) {
                case 0:  next = 1;  break;   // auto -> 1
                case 1:  next = 6;  break;
                case 6:  next = 11; break;
                default: next = 0;  break;   // 11 -> auto
            }
            apply_channel_setting(next);
        } else {
            last_click_ms_ = now;
        }
    }
#else
    (void)now;
#endif
}

void RepeaterMode::apply_channel_setting(uint8_t setting) {
    channel_setting_ = setting;
    persistence::save_repeater_channel(setting);
    if (setting == 0) {
        locked_                = false;
        scan_idx_              = 0;
        scan_probe_started_ms_ = millis();
        start_radio(kScanChannels[0]);
    } else {
        locked_ = true;
        start_radio(setting);
    }
    confirm_until_ms_ = millis() + kConfirmMs;
}

void RepeaterMode::service_scan(uint32_t now) {
    if (channel_setting_ != 0) return;   // fixed channel: never scans

    if (!locked_) {
        // Probing: if the current channel stayed silent past the dwell,
        // advance to the next candidate.
        if (now - scan_probe_started_ms_ >= kScanDwellMs) {
            scan_idx_ = (scan_idx_ + 1) % 3;
            start_radio(kScanChannels[scan_idx_]);
            scan_probe_started_ms_ = now;
        }
    } else if (now - last_rx_ms_ >= kScanRelockMs) {
        // Locked but the channel went quiet: drop the lock and resume
        // scanning from the next candidate.
        locked_                = false;
        scan_idx_              = (scan_idx_ + 1) % 3;
        start_radio(kScanChannels[scan_idx_]);
        scan_probe_started_ms_ = now;
    }
}

void RepeaterMode::emit_census(uint32_t now) {
    if (now - last_census_ms_ < kCensusIntervalMs) return;
    last_census_ms_ = now;
    if (!radio_up_) return;
    if (channel_setting_ == 0 && !locked_) return;   // still scanning

    auto* radio = hal::HAL::esp_now();
    if (radio == nullptr) return;

    te::Header hdr{};
    // Broadcast/anonymous source id, deliberately: both TofuLock ports
    // (Lume C++ and badge Python) hard-reject kBroadcastSourceId before
    // establishing a lock, so census can never steal a receiver's
    // Director lock, refresh its liveness clock, or reach the badge's
    // repeater FSM. The Director's census intake doesn't consult the
    // header source id - identity rides in the payload uid.
    hdr.source_id       = te::kBroadcastSourceId;
    hdr.sequence_number = 0;   // census is unsequenced
    hdr.hop_count       = 0;

    te::RepeaterHeartbeatPayload p{};
    p.uid[0]  = uid_[0];
    p.uid[1]  = uid_[1];
    p.uid[2]  = uid_[2];
    p.channel = active_channel_;
    const uint32_t up = (now - boot_ms_) / 1000u;
    p.uptime_s      = (up > 0xFFFFu) ? 0xFFFFu : static_cast<uint16_t>(up);
    p.relayed_count = relayed_count_;

    uint8_t buf[te::kHeaderSize + te::kRepeaterHeartbeatPayloadLen];
    const size_t n = te::encode_repeater_heartbeat(buf, sizeof(buf), hdr, p);
    if (n > 0) radio->send_broadcast(buf, n);
}

void RepeaterMode::update_led(uint32_t now) {
#ifdef ARDUINO
    constexpr int kPin = 35;   // AtomS3 onboard WS2812

    // Channel-change confirmation: bright solid colour of the new setting.
    if (now < confirm_until_ms_) {
        uint8_t r, g, b;
        channel_colour(channel_setting_, r, g, b);   // 0 -> white (auto)
        neopixelWrite(kPin, static_cast<uint8_t>(r * 2),
                            static_cast<uint8_t>(g * 2),
                            static_cast<uint8_t>(b * 2));
        return;
    }
    if (!radio_up_) {
        neopixelWrite(kPin, 40, 0, 0);      // red: radio down
        return;
    }
    if (channel_setting_ == 0 && !locked_) {
        neopixelWrite(kPin, 30, 12, 0);     // amber: scanning
        return;
    }
    if (now - last_relay_ms_ < kRelayFlashMs) {
        neopixelWrite(kPin, 0, 40, 40);     // cyan: relaying right now
        return;
    }
    // Idle / online: dim colour of the channel we're relaying on.
    uint8_t r, g, b;
    channel_colour(active_channel_, r, g, b);
    neopixelWrite(kPin, r, g, b);
#else
    (void)now;
#endif
}

bool RepeaterMode::seen_recently(uint8_t src, uint8_t seq) const {
    if (seq == 0) return false;   // seq 0 = sequencing disabled, never dedup
    for (size_t i = 0; i < kDedupRingSize; ++i) {
        if (dedup_ring_[i].source_id == src &&
            dedup_ring_[i].sequence_number == seq) {
            return true;
        }
    }
    return false;
}

void RepeaterMode::mark_seen(uint8_t src, uint8_t seq) {
    if (seq == 0) return;
    dedup_ring_[dedup_head_] = DedupEntry{src, seq};
    dedup_head_ = (dedup_head_ + 1) % kDedupRingSize;
}

}  // namespace modes
}  // namespace nocturnation
