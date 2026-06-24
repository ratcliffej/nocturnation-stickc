// M5 AtomS3 (Atomic PoE) ESPNow backend.
//
// Identical in shape to ESPNowStickCS3 - the esp_now / esp_wifi APIs are
// the same on every ESP32-S3, and ESP-NOW runs on the WiFi radio in
// WIFI_STA mode WITHOUT associating to any AP. That is exactly why this
// board works: the W5500 Ethernet (console/sACN link) lives on a separate
// SPI peripheral, so the radio is free for the fleet broadcast and there
// is no WiFi/ESP-NOW channel-coexistence problem.
//
// See src/hal_stickcs3/esp_now_stickcs3.h for the full design notes.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ESPNowAtomS3Poe : public ESPNow {
public:
    ESPNowAtomS3Poe();

    bool begin(uint8_t wifi_channel) override;
    void end() override;
    bool set_channel(uint8_t wifi_channel) override;

    bool send_broadcast(const uint8_t* data, size_t len) override;
    bool send_to(const uint8_t mac[6], const uint8_t* data, size_t len) override;

    void set_recv_callback(RecvCallback cb) override;

    // Internal: invoked from the static C-style ESP-NOW callback. Public
    // so the file-scope trampoline in the .cpp can reach it.
    void dispatch_recv(const uint8_t* mac_addr,
                       const uint8_t* data, int len);

private:
    bool          running_ = false;
    uint8_t       channel_ = 0;
    RecvCallback  callback_;
};

}  // namespace hal
}  // namespace nocturnation
