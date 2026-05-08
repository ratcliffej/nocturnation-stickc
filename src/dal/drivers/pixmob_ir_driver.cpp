#include "pixmob_ir_driver.h"
#include "hal/hal.h"
#include "pixmob_protocol.h"

#ifdef ARDUINO
#include <Arduino.h>
#endif

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

bool PixMobIRDriver::send(uint8_t /*group_id*/, const AssignDeviceGroupEvent& ev) {
    // PixMob group assignment is a 2-command sequence:
    //   1. SetGroupId(slot, new_id)  - writes the value into the EEPROM slot
    //   2. SetGroupSel(slot)         - activates that slot's value as the
    //                                  bracelet's current group filter
    // The first command alone leaves the new value stored but unused.
    // Reference: jamesw343/PixMob_IR pixmob_ir_protocol_examples.py "Group
    // Id" example, where the comment on SetGroupSel reads "this changes
    // the PixMob's group id to 22". The dispatch group_id is not used
    // here - both commands fire as broadcast (restrictGroupId=0) so the
    // target bracelet (physically isolated per protocol) receives them.
    auto* ir = hal::HAL::ir_tx();
    if (!ir) return false;
    if (ev.new_group_id < 1 || ev.new_group_id > 31) return false;

    uint16_t buf[kPulseBufSize];

    // Step 1: Write new_group_id into slot 0.
    size_t n = pixmob::buildSetGroupId(
        buf, kPulseBufSize,
        /*groupSel=*/0, ev.new_group_id, /*restrictGroupId=*/0);
    if (n == 0) return false;
    ir->send_raw(buf, n, 38);

    // Brief gap so the bracelet finishes processing the first command
    // before the second one starts arriving on the wire. 30 ms is well
    // under any user-perceived delay and gives the IR receiver time to
    // re-arm after the trailing pulse of the first transmission.
#ifdef ARDUINO
    ::delay(30);
#endif

    // Step 2: Activate slot 0 - commits new_group_id as the active filter.
    n = pixmob::buildSetGroupSel(
        buf, kPulseBufSize,
        /*groupSel=*/0, /*restrictGroupId=*/0);
    if (n == 0) return false;
    ir->send_raw(buf, n, 38);
    return true;
}

}  // namespace dal
}  // namespace nocturnation
