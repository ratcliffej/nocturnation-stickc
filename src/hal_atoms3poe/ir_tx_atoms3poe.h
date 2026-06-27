// AtomS3-PoE IRTx backend - external IR emitter on the Grove connector.
//
// The Atomic PoE base physically covers the AtomS3's built-in IR LED, so the
// Director drives an external IR unit on the Grove port instead. G2 (GPIO 2)
// is the Grove TX line (G1 is RX). Same IRremoteESP8266 path as the Stick
// backend - only the pin differs - so the PixMob IR encoder and the DAL
// "ir-pixmob" loopback fire here automatically once IRTx is declared.

#pragma once

#include "hal/hal.h"
#include <IRremoteESP8266.h>
#include <IRsend.h>

namespace nocturnation {
namespace hal {

class IRTxAtomS3Poe : public IRTx {
public:
    IRTxAtomS3Poe();

    void begin() override;
    void send_raw(const uint16_t* pulses_us, size_t count,
                  uint16_t carrier_khz) override;

private:
    static constexpr uint16_t kIRPin = 2;   // Grove G2 (OUT) on the Atomic PoE base
    IRsend irsend_;
};

}  // namespace hal
}  // namespace nocturnation
