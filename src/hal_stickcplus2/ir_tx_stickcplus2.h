// M5StickC Plus2 IRTx backend.
//
// Owns an IRsend instance for a single IR LED. All IR transmissions in the
// firmware go through this backend - callers above the HAL never touch
// IRremoteESP8266 directly.
//
// The pin is a constructor argument so one class can serve both emitters on
// the Plus2: the built-in LED on GPIO 19 (kInternalIRPin) and an optional
// external M5Stack IR unit wired to the GPIO 26 header pin (kExternalIRPin).
// The HAL backend owns one instance per emitter.

#pragma once

#include "hal/hal.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

namespace nocturnation {
namespace hal {

class IRTxStickCplus2 : public IRTx {
public:
    // Built-in IR LED, and the external-unit header pin (DAT on GPIO 26).
    static constexpr uint16_t kInternalIRPin = 19;
    static constexpr uint16_t kExternalIRPin = 26;

    explicit IRTxStickCplus2(uint16_t pin = kInternalIRPin);

    void begin() override;
    void send_raw(const uint16_t* pulses_us, size_t count,
                  uint16_t carrier_khz) override;

private:
    IRsend irsend_;
};

}  // namespace hal
}  // namespace nocturnation
