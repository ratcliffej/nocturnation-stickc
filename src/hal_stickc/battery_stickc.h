// M5StickC Plus2 Battery backend.
//
// Wraps M5.Power (AXP192 on the Plus2).

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class BatteryStickC : public Battery {
public:
    int   level_percent() override;
    float voltage() override;
    bool  is_charging() override;
};

}  // namespace hal
}  // namespace nocturnation
