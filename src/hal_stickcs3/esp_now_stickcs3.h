// M5StickS3 ESPNow backend.
//
// Same shape as ESPNowStickCplus2 - the underlying esp_now / esp_wifi
// APIs are identical between ESP32 and ESP32-S3, so the body is
// effectively duplicated. The HAL discipline of one backend per binary
// makes per-host class names worth keeping despite the duplication; if
// it grows tedious we can DRY via a shared internal helper later.
//
// See src/hal_stickcplus2/esp_now_stickcplus2.h for full design notes.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ESPNowStickCS3 : public ESPNow {
public:
    ESPNowStickCS3();

    bool begin(uint8_t wifi_channel) override;
    void end() override;

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
