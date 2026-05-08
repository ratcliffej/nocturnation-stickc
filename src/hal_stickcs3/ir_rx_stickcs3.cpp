#include "ir_rx_stickcs3.h"
#include <Arduino.h>

namespace nocturnation {
namespace hal {

IRRxStickCS3::IRRxStickCS3()
    : irrecv_(kIRPin, kCaptureBufSize, kFrameTimeoutMs, /*save_buffer=*/false) {}

void IRRxStickCS3::begin() {
    if (running_) return;
    irrecv_.enableIRIn();
    running_ = true;
}

void IRRxStickCS3::end() {
    if (!running_) return;
    irrecv_.disableIRIn();
    running_ = false;
}

void IRRxStickCS3::set_pulses_callback(PulsesCallback cb) {
    callback_ = cb;
}

void IRRxStickCS3::poll() {
    if (!running_) return;
    if (!irrecv_.decode(&results_)) return;

    // results_.rawbuf[0] is the gap before the first edge; skip it.
    // Subsequent entries alternate mark/space in kRawTick (50us) units.
    if (callback_ && results_.rawlen > 1) {
        size_t count = static_cast<size_t>(results_.rawlen) - 1;
        if (count > kCaptureBufSize) count = kCaptureBufSize;
        for (size_t i = 0; i < count; ++i) {
            scratch_pulses_us_[i] =
                static_cast<uint16_t>(results_.rawbuf[i + 1] * kRawTick);
        }
        IRPulses pulses{};
        pulses.timestamp_ms = millis();
        pulses.pulses_us    = scratch_pulses_us_;
        pulses.count        = count;
        callback_(pulses);
    }

    irrecv_.resume();
}

}  // namespace hal
}  // namespace nocturnation
