// PixMobIRDriver - DAL driver for the "ir-pixmob" transport.
//
// Translates RgbPulseEvents into PixMob IR pulse trains via the
// pixmob::buildSingleColor encoder, then transmits them through the HAL's
// IR transmitter(s).
//
// The encoder runs once per command; the resulting pulse train is fanned
// out to whichever emitter(s) are enabled - the built-in LED
// (hal::HAL::ir_tx(), "internal") and/or an external unit
// (hal::HAL::ir_tx_ext(), "external", Plus2 GPIO 26). Both on = both fire
// back-to-back, to boost coverage. The encoder/transmit split is unchanged;
// only the output stage fans out.
//
// Registered automatically by DAL::begin() when hal::HAL has IRTx. The
// driver is the join key for any active device whose profile declares
// transport = "ir-pixmob" (e.g. PixMobX4Gen3_1).

#pragma once

#include "dal/dal.h"

namespace nocturnation {
namespace dal {

class PixMobIRDriver : public Driver {
public:
    const char* transport_name() const override { return "ir-pixmob"; }

    bool begin() override;
    void loop_tick() override {}

    bool send(uint8_t group_id, const RgbPulseEvent&) override;
    bool send(uint8_t group_id, const AssignDeviceGroupEvent&) override;

    // Emitter selection. The encoded frame is fanned out to each enabled
    // emitter. Internal is the built-in IR LED (default on, preserving the
    // pre-existing single-emitter behaviour); external is an optional unit
    // on hal::HAL::ir_tx_ext() (default off). Loaded from NVS at boot and
    // updated live from the IR config menu.
    void set_internal_enabled(bool e) { internal_enabled_ = e; }
    bool internal_enabled() const { return internal_enabled_; }
    void set_external_enabled(bool e) { external_enabled_ = e; }
    bool external_enabled() const { return external_enabled_; }

private:
    // Fan one encoded pulse train out to every enabled emitter. Each
    // emitter is null-checked, so an enabled-but-absent external sink is a
    // no-op rather than a crash.
    void transmit(const uint16_t* pulses_us, size_t count);

    bool internal_enabled_ = true;
    bool external_enabled_ = false;
};

PixMobIRDriver* pixmob_ir_driver_instance();

}  // namespace dal
}  // namespace nocturnation
