#include "ir_tx_stickcs3.h"

namespace nocturnation {
namespace hal {

IRTxStickCS3::IRTxStickCS3() : irsend_(kIRPin) {}

void IRTxStickCS3::begin() {
    irsend_.begin();
}

void IRTxStickCS3::send_raw(const uint16_t* pulses_us, size_t count,
                            uint16_t carrier_khz) {
    if (!pulses_us || count == 0) return;
    irsend_.sendRaw(pulses_us, count, carrier_khz);
}

}  // namespace hal
}  // namespace nocturnation
