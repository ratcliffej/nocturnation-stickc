// DmxUsbCdcAdapter - Arduino USB-CDC wrapper that pumps bytes into a
// DmxInputParser.
//
// Epic 7 B1.5 (follow-up to B1's pure-state-machine parser). The
// parser is host-testable in isolation (test_dmx_input_parser); this
// adapter is the Arduino seam that takes bytes off the live Serial
// stream and feeds them into the parser one at a time. Lifecycle
// methods (begin / end) handle the baud-rate switch between the
// 115 200 boot/console rate and the 921 600 Enttec Pro DMX rate -
// the StickC's single USB-CDC peripheral can't carry both at once,
// so the application must call end() to restore the console before
// shipping any println() debug output (or just accept the loss while
// DMX mode is active per Epic 7 Q1).
//
// The adapter does not register with DAL or any mode FSM directly.
// Epic 7 B3 (DmxBridge mode) instantiates one in its enter() and
// drops it in exit(), so the lifecycle matches the mode's.

#pragma once

#include <cstddef>
#include <cstdint>

#include "dmx_input_parser.h"

namespace nocturnation {
namespace dal {

// The adapter operates against the global Arduino `Serial` object,
// whose type differs by board (`HardwareSerial` on the Plus2's UART
// USB-CDC vs `HWCDC` on the S3's native USB-CDC). Rather than carry a
// templated reference, the adapter reaches `Serial` directly from
// the impl's translation unit - one less generic to maintain, and
// the binding to the global is fine for v1 (we never want to bridge
// DMX over anything other than the StickC's primary USB-CDC).

class DmxUsbCdcAdapter {
public:
    // Wire baud for the StickC. Was 921600 (the conventional Enttec
    // DMX USB Pro rate) until 2026-06-18; the shared shim's
    // nocturnation_dmx/enttec_pro.py was lowered to 460 800 as a
    // Plus2 accommodation (the ESP32-PICO-V3-02 is single-core and
    // shares its UART RX interrupt with ESP-NOW TX + the LCD; at 921
    // 600 the UART hardware FIFO only holds ~1.4 ms of inbound
    // traffic, shorter than the IRQ-off windows ESP-NOW briefly opens
    // during radio handoff, so bytes drop silently mid-frame and the
    // parser sits in WaitStart with r:0 o:0). 460 800 doubles the
    // FIFO lifetime to ~2.8 ms while keeping 50% headroom over
    // 44 fps DMX. The S3's native USB-CDC ignores the baud value
    // entirely so this change is invisible there. The firmware side
    // MUST match the shim side - mismatched bauds means USB bytes
    // arrive corrupted and the parser never sees a valid frame.
    // Configurable in begin() if a future test rig wants something
    // different.
    static constexpr uint32_t kDefaultBaud = 460800;

    DmxUsbCdcAdapter();

    // Open the serial at the DMX baud and reset the parser. Idempotent:
    // calling begin() while already active simply re-runs the baud
    // change and parser reset, which the caller may want after a
    // mode re-entry.
    void begin(uint32_t baud = kDefaultBaud);

    // Close the serial. The caller is responsible for re-opening the
    // console at 115 200 (or whatever) afterwards. Idempotent.
    void end();

    // Pump pending bytes from Serial through the parser. Returns the
    // number of complete frames observed this call (0 if no frame
    // crossed a boundary; >= 1 if one or more landed). Call from the
    // mode's loop_tick at the usual cadence; the parser is stateless
    // between calls so any pump frequency works, but ~5-10 ms is a
    // good default to keep latency under the 50 ms slider-to-Lume
    // budget.
    size_t poll();

    // Adapter is active between begin() and end().
    bool active() const { return active_; }

    // Direct access to the underlying parser for reading the most
    // recently completed frame.
    DmxInputParser&       parser()       { return parser_; }
    const DmxInputParser& parser() const { return parser_; }

    // Total bytes read from Serial since the last begin(). Useful for
    // diagnostics on the bench (was traffic flowing at all?).
    uint32_t bytes_read() const { return bytes_read_; }

private:
    DmxInputParser  parser_;
    bool            active_     = false;
    uint32_t        bytes_read_ = 0;
};

}  // namespace dal
}  // namespace nocturnation
