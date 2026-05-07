#include "pixmob_ir_driver.h"
#include "hal/hal.h"
#include "pixmob_protocol.h"

namespace nocturnation {
namespace dal {

namespace {
PixMobIRDriver s_instance;

// Working buffer for the pulse train. 80 entries is plenty for any 9-byte
// PixMob command (the longest transmission is well under that). Sized to
// match the prototype's irBuf so behaviour is byte-identical.
constexpr size_t kPulseBufSize = 80;
}  // namespace

PixMobIRDriver* pixmob_ir_driver_instance() { return &s_instance; }

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

bool PixMobIRDriver::begin() {
    // Refuse registration if there is no IRTx to drive.
    return hal::HAL::ir_tx() != nullptr;
}

// -----------------------------------------------------------------------------
// Output dispatch
// -----------------------------------------------------------------------------

bool PixMobIRDriver::send(uint8_t group_id, const RgbPulseEvent& ev) {
    auto* ir = hal::HAL::ir_tx();
    if (!ir) return false;

    uint16_t buf[kPulseBufSize];
    const size_t n = pixmob::buildSingleColor(
        buf, kPulseBufSize,
        ev.r, ev.g, ev.b,
        ev.attack, ev.sustain, ev.release,
        ev.chance,
        group_id);
    if (n == 0) return false;

    // 38 kHz carrier per the PixMob protocol.
    ir->send_raw(buf, n, 38);
    return true;
}

}  // namespace dal
}  // namespace nocturnation
