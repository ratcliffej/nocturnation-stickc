// PixMobIRDriver - DAL driver for the "ir-pixmob" transport.
//
// Translates RgbPulseEvents into PixMob IR pulse trains via the
// pixmob::buildSingleColor encoder, then transmits them through
// hal::HAL::ir_tx().
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
};

PixMobIRDriver* pixmob_ir_driver_instance();

}  // namespace dal
}  // namespace nocturnation
