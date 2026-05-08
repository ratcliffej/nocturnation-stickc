// M5StickC Plus2 IRTx backend.
//
// Owns the only IRsend instance for GPIO 19 (the StickC Plus2's built-in IR
// LED). All IR transmissions in the firmware now go through this backend -
// callers above the HAL never touch IRremoteESP8266 directly. This is the
// commit that consolidates IRsend ownership; main.cpp's previous global
// `IRsend irsend(IR_PIN)` is removed.

#pragma once

#include "hal/hal.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

namespace nocturnation {
namespace hal {

class IRTxStickCplus2 : public IRTx {
public:
    IRTxStickCplus2();

    void begin() override;
    void send_raw(const uint16_t* pulses_us, size_t count,
                  uint16_t carrier_khz) override;

private:
    static constexpr uint16_t kIRPin = 19;
    IRsend irsend_;
};

}  // namespace hal
}  // namespace nocturnation
