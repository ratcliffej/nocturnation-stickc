#include "ir_tx_atoms3poe.h"

namespace nocturnation {
namespace hal {

IRTxAtomS3Poe::IRTxAtomS3Poe() : irsend_(kIRPin) {}

void IRTxAtomS3Poe::begin() {
    irsend_.begin();
}

void IRTxAtomS3Poe::send_raw(const uint16_t* pulses_us, size_t count,
                             uint16_t carrier_khz) {
    if (!pulses_us || count == 0) return;
    irsend_.sendRaw(pulses_us, count, carrier_khz);
}

}  // namespace hal
}  // namespace nocturnation
