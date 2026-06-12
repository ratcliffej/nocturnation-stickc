#include "esp_now_stickcs3.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <cstring>

namespace nocturnation {
namespace hal {

namespace {

constexpr uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
constexpr int8_t  kTxPower21dBm = 84;   // 0.25 dBm units; 84 == 21.0 dBm

ESPNowStickCS3* s_instance = nullptr;

void IRAM_ATTR static_recv_cb(const uint8_t* mac_addr,
                              const uint8_t* data, int len) {
    if (s_instance) s_instance->dispatch_recv(mac_addr, data, len);
}

}  // namespace

ESPNowStickCS3::ESPNowStickCS3() = default;

bool ESPNowStickCS3::begin(uint8_t wifi_channel) {
    if (running_) return true;

    WiFi.mode(WIFI_STA);
    // Disable modem sleep. Arduino-ESP32's STA default is
    // WIFI_PS_MIN_MODEM, which duty-cycles the radio and lets it sleep
    // through short ESP-NOW bursts (the Director sends 3x retransmits
    // within ~2 ms). Receivers must call setSleep(false) for reliable
    // receive. Any future light-sleep work must keep the radio awake
    // for heartbeat windows or this bug returns.
    WiFi.setSleep(false);
    WiFi.disconnect();

    if (esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    channel_ = wifi_channel;

    esp_wifi_set_max_tx_power(kTxPower21dBm);

    if (esp_now_init() != ESP_OK) {
        return false;
    }
    s_instance = this;
    esp_now_register_recv_cb(static_recv_cb);

    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, kBroadcastMac, sizeof(peer.peer_addr));
    peer.channel = wifi_channel;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    running_ = true;
    return true;
}

void ESPNowStickCS3::end() {
    if (!running_) return;
    esp_now_unregister_recv_cb();
    esp_now_del_peer(kBroadcastMac);
    esp_now_deinit();
    s_instance = nullptr;
    running_ = false;
}

bool ESPNowStickCS3::set_channel(uint8_t wifi_channel) {
    if (!running_) return false;
    if (esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        return false;
    }
    channel_ = wifi_channel;
    esp_now_peer_info_t peer{};
    std::memcpy(peer.peer_addr, kBroadcastMac, sizeof(peer.peer_addr));
    peer.channel = wifi_channel;
    peer.encrypt = false;
    esp_now_mod_peer(&peer);
    return true;
}

bool ESPNowStickCS3::send_broadcast(const uint8_t* data, size_t len) {
    if (!running_ || data == nullptr || len == 0) return false;
    return esp_now_send(kBroadcastMac, data, len) == ESP_OK;
}

bool ESPNowStickCS3::send_to(const uint8_t mac[6],
                             const uint8_t* data, size_t len) {
    if (!running_ || mac == nullptr || data == nullptr || len == 0) {
        return false;
    }
    return esp_now_send(mac, data, len) == ESP_OK;
}

void ESPNowStickCS3::set_recv_callback(RecvCallback cb) {
    callback_ = cb;
}

void ESPNowStickCS3::dispatch_recv(const uint8_t* mac_addr,
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
