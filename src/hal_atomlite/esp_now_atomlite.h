// M5Atom Lite ESPNow backend (Epic 12 follow-on).
//
// API-identical to ESPNowStickCplus2; the underlying Arduino-ESP32 +
// ESP-IDF calls are board-agnostic so the implementation is a straight
// port. Kept in the hal_atomlite/ tree (rather than a shared file) for
// consistency with the rest of the HAL discipline: one backend folder
// per host, no cross-folder file-sharing.
//
// Receive uses the legacy esp_now_recv_cb_t signature available under
// arduino-esp32 6.7.0; RSSI is fixed at -128 ("unknown"). A future
// promiscuous-mode sniffer would lift that on both this and the
// stickcplus2 backend in lock-step.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class ESPNowAtomLite : public ESPNow {
public:
    ESPNowAtomLite();

    bool begin(uint8_t wifi_channel) override;
    void end() override;
    bool set_channel(uint8_t wifi_channel) override;

    bool send_broadcast(const uint8_t* data, size_t len) override;
    bool send_to(const uint8_t mac[6], const uint8_t* data, size_t len) override;

    void set_recv_callback(RecvCallback cb) override;

    void dispatch_recv(const uint8_t* mac_addr,
                       const uint8_t* data, int len);

private:
    bool          running_ = false;
    uint8_t       channel_ = 0;
    RecvCallback  callback_;
};

}  // namespace hal
}  // namespace nocturnation
