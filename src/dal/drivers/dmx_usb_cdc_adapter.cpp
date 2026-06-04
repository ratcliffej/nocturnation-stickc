// DmxUsbCdcAdapter implementation - Epic 7 B1.5.
//
// Binds to the global Arduino `Serial` object. Its type differs per
// board (HardwareSerial on Plus2; HWCDC on S3 with ARDUINO_USB_CDC_ON_BOOT)
// but both expose begin(baud) / end() / available() / read() so the
// calls below are source-compatible.

#include "dmx_usb_cdc_adapter.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

namespace nocturnation {
namespace dal {

DmxUsbCdcAdapter::DmxUsbCdcAdapter() = default;

void DmxUsbCdcAdapter::begin(uint32_t baud) {
#ifdef ARDUINO
    // Close any prior open (no-op if Serial was never begun) so the
    // baud change reliably takes effect.
    Serial.end();
    Serial.begin(baud);
#else
    (void)baud;
#endif
    parser_.reset();
    parser_.reset_counters();
    bytes_read_ = 0;
    active_     = true;
}

void DmxUsbCdcAdapter::end() {
#ifdef ARDUINO
    Serial.end();
#endif
    active_ = false;
}

size_t DmxUsbCdcAdapter::poll() {
    if (!active_) return 0;
#ifdef ARDUINO
    size_t frames_this_call = 0;
    // Drain everything pending in one call - Serial.available() returns
    // the receive buffer fill, which can be hundreds of bytes if poll
    // hasn't run in a while. The parser is cheap per byte (one switch +
    // one assignment); a few hundred bytes is fine inside a 50 ms tick.
    while (Serial.available() > 0) {
        const int b = Serial.read();
        if (b < 0) break;   // belt-and-braces; available() said >0
        ++bytes_read_;
        if (parser_.feed_byte(static_cast<uint8_t>(b))
            == DmxParseResult::FrameComplete) {
            ++frames_this_call;
        }
    }
    return frames_this_call;
#else
    return 0;
#endif
}

}  // namespace dal
}  // namespace nocturnation
