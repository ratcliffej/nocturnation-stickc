// M5StickC Plus2 ESPNow backend.
//
// Wraps Arduino-ESP32's ESP-NOW + WiFi APIs. begin(channel) brings WiFi
// up in STA mode (no AP connect), pins the radio to the chosen channel,
// initialises ESP-NOW, registers a broadcast peer, and sets +21 dBm
// transmit power per architecture spec §4.6. send_broadcast / send_to
// transmit raw bytes; orchestration owns frame encoding (see
// include/transport/espnow/frame.h).
//
// Receive runs through the legacy esp_now_recv_cb_t signature available
// in arduino-esp32 v6.7.0, which doesn't carry RSSI - the ESPNowMessage
// fired to subscribers has rssi = -128 ("unknown"). Block 7 of Epic 4
// will pick up RSSI via the promiscuous-mode sniffer pattern.
//
// One backend instance is linked per binary (HAL static-facade discipline),
// so the static C callback dispatches via a file-scope instance pointer.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ESPNowStickCplus2 : public ESPNow {
public:
    ESPNowStickCplus2();

    bool begin(uint8_t wifi_channel) override;
    void end() override;
    bool set_channel(uint8_t wifi_channel) override;

    bool send_broadcast(const uint8_t* data, size_t len) override;
    bool send_to(const uint8_t mac[6], const uint8_t* data, size_t len) override;

    void set_recv_callback(RecvCallback cb) override;

    // Internal: invoked from the static C-style ESP-NOW callback. Public
    // so the file-scope trampoline in the .cpp can reach it; not part of
    // the HAL contract.
    void dispatch_recv(const uint8_t* mac_addr,
                       const uint8_t* data, int len);

private:
    bool          running_ = false;
    uint8_t       channel_ = 0;
    RecvCallback  callback_;
};

}  // namespace hal
}  // namespace nocturnation
