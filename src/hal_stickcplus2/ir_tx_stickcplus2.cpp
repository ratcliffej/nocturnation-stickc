#include "ir_tx_stickcplus2.h"

namespace nocturnation {
namespace hal {

IRTxStickCplus2::IRTxStickCplus2() : irsend_(kIRPin) {}

void IRTxStickCplus2::begin() {
    irsend_.begin();
}

void IRTxStickCplus2::send_raw(const uint16_t* pulses_us, size_t count,
                          uint16_t carrier_khz) {
    if (!pulses_us || count == 0) return;
    irsend_.sendRaw(pulses_us, count, carrier_khz);
}

}  // namespace hal
}  // namespace nocturnation
