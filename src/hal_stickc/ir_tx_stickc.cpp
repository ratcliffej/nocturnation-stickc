#include "ir_tx_stickc.h"

namespace nocturnation {
namespace hal {

IRTxStickC::IRTxStickC() : irsend_(kIRPin) {}

void IRTxStickC::begin() {
    irsend_.begin();
}

void IRTxStickC::send_raw(const uint16_t* pulses_us, size_t count,
                          uint16_t carrier_khz) {
    if (!pulses_us || count == 0) return;
    irsend_.sendRaw(pulses_us, count, carrier_khz);
}

}  // namespace hal
}  // namespace nocturnation
