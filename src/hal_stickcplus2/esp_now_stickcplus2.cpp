#include "esp_now_stickcplus2.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>

namespace nocturnation {
namespace hal {

namespace {

// Broadcast destination per spec §4.3 - all-receivers ESP-NOW MAC.
constexpr uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

// +21 dBm transmit power expressed in 0.25 dBm units. arduino-esp32's
// esp_wifi_set_max_tx_power expects this scale; 84 == 21.0 dBm. The chip
// silently clamps to its compiled ceiling so we don't error-check the
// return of set_max_tx_power.
constexpr int8_t kTxPower21dBm = 84;

// Single-instance trampoline. The HAL discipline of one backend per binary
// (duplicate-symbol guarantee) means there's exactly one ESPNowStickCplus2
// alive at a time, so a file-scope pointer is safe.
ESPNowStickCplus2* s_instance = nullptr;

void IRAM_ATTR static_recv_cb(const uint8_t* mac_addr,
                              const uint8_t* data, int len) {
    if (s_instance) s_instance->dispatch_recv(mac_addr, data, len);
}

}  // namespace

ESPNowStickCplus2::ESPNowStickCplus2() = default;

bool ESPNowStickCplus2::begin(uint8_t wifi_channel) {
    if (running_) return true;

    // 1. Bring WiFi up in station mode without connecting to any AP.
    //    ESP-NOW shares the WiFi radio; we just need the MAC layer up.
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();    // clear any stored credentials' auto-connect

    // 2. Pin the radio to the requested channel. WIFI_SECOND_CHAN_NONE
    //    forces 20 MHz operation (no HT40 secondary), matching what the
    //    upstream ESP-NOW examples assume.
    if (esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    channel_ = wifi_channel;

    // 3. Set max TX power. +21 dBm is our default (architecture §4.6); a
    //    later Epic may expose this for slaves/repeaters.
    esp_wifi_set_max_tx_power(kTxPower21dBm);

    // 4. Initialise ESP-NOW and register the legacy receive callback.
    if (esp_now_init() != ESP_OK) {
        return false;
    }
    s_instance = this;
    esp_now_register_recv_cb(static_recv_cb);

    // 5. Register the broadcast peer. ESP-NOW requires every destination -
    //    including the broadcast MAC - to be added as a peer before sends.
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, kBroadcastMac, sizeof(peer.peer_addr));
    peer.channel = wifi_channel;
    peer.encrypt = false;
    if (esp_now_add_peer(&peer) != ESP_OK) {
        // Already-registered is acceptable on a re-init path, but a clean
        // first boot should not hit this. Keep going either way.
    }

    running_ = true;
    return true;
}

void ESPNowStickCplus2::end() {
    if (!running_) return;
    esp_now_unregister_recv_cb();
    esp_now_del_peer(kBroadcastMac);
    esp_now_deinit();
    s_instance = nullptr;
    running_ = false;
}

bool ESPNowStickCplus2::send_broadcast(const uint8_t* data, size_t len) {
    if (!running_ || data == nullptr || len == 0) return false;
    return esp_now_send(kBroadcastMac, data, len) == ESP_OK;
}

bool ESPNowStickCplus2::send_to(const uint8_t mac[6],
                                const uint8_t* data, size_t len) {
    if (!running_ || mac == nullptr || data == nullptr || len == 0) {
        return false;
    }
    // Caller is responsible for having added this peer; we don't lazily
    // add unicast peers because ESP-NOW caps total peer count at 20 and
    // we want the caller to manage that explicitly.
    return esp_now_send(mac, data, len) == ESP_OK;
}

void ESPNowStickCplus2::set_recv_callback(RecvCallback cb) {
    callback_ = cb;
}

void ESPNowStickCplus2::dispatch_recv(const uint8_t* mac_addr,
                                      const uint8_t* data, int len) {
    if (!callback_ || mac_addr == nullptr || data == nullptr || len <= 0) {
        return;
    }
    ESPNowMessage msg{};
    msg.timestamp_ms = millis();
    std::memcpy(msg.peer_mac, mac_addr, sizeof(msg.peer_mac));
    msg.data = data;
    msg.len  = static_cast<size_t>(len);
    msg.rssi = -128;   // legacy callback - RSSI captured via Block 7's sniffer
    callback_(msg);
}

}  // namespace hal
}  // namespace nocturnation
