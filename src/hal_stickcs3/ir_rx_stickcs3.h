// M5StickS3 IRRx backend.
//
// Captures raw IR pulse trains on GPIO 42 (the S3's built-in IR receiver,
// absent on the Plus2). Uses IRremoteESP8266's IRrecv in raw-buffer mode -
// no per-protocol decoding here; that's the orchestration layer's job.
//
// Per the StickS3 hardware note (docs.m5stack.com): when the IR receiver is
// in use the AW8737 speaker amplifier must be off, otherwise IR signals
// cannot be received properly. Orchestration that wants both speaker and
// IR Rx active simultaneously will need to mediate; this backend doesn't
// touch the speaker.

#pragma once

#include "hal/hal.h"
#include <IRremoteESP8266.h>
#include <IRrecv.h>

namespace nocturnation {
namespace hal {

class IRRxStickCS3 : public IRRx {
public:
    IRRxStickCS3();

    void begin() override;
    void end() override;
    void set_pulses_callback(PulsesCallback cb) override;

    // Backend-specific - called from HAL::loop_tick() in hal_stickcs3.cpp.
    void poll();

private:
    static constexpr uint16_t kIRPin           = 42;
    static constexpr uint16_t kCaptureBufSize  = 1024;  // max edges per capture
    static constexpr uint8_t  kFrameTimeoutMs  = 50;    // silence to mark frame end

    bool           running_ = false;
    PulsesCallback callback_;

    IRrecv         irrecv_;
    decode_results results_{};
    uint16_t       scratch_pulses_us_[kCaptureBufSize];
};

}  // namespace hal
}  // namespace nocturnation
