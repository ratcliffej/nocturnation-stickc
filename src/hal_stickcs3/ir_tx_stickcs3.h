// M5StickS3 IRTx backend.
//
// Owns the only IRsend instance for the StickS3's built-in IR LED on
// GPIO 46 (vs the Plus2's GPIO 19). All IR transmissions in the firmware
// go through this backend - callers above the HAL never touch
// IRremoteESP8266 directly.

#pragma once

#include "hal/hal.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

namespace nocturnation {
namespace hal {

class IRTxStickCS3 : public IRTx {
public:
    IRTxStickCS3();

    void begin() override;
    void send_raw(const uint16_t* pulses_us, size_t count,
                  uint16_t carrier_khz) override;

private:
    static constexpr uint16_t kIRPin = 46;
    IRsend irsend_;
};

}  // namespace hal
}  // namespace nocturnation
