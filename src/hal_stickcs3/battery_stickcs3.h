// M5StickS3 Battery backend.
//
// Wraps M5.Power. Behaviour identical to the Plus2 backend - M5Unified
// abstracts away the underlying PMU difference (StickS3 uses a different
// power IC from the Plus2's AXP192 but the M5.Power query surface is the
// same).

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class BatteryStickCS3 : public Battery {
public:
    int   level_percent() override;
    float voltage() override;
    bool  is_charging() override;
};

}  // namespace hal
}  // namespace nocturnation
