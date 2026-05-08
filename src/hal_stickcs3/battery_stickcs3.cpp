#include "battery_stickcs3.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

int BatteryStickCS3::level_percent() {
    return M5.Power.getBatteryLevel();
}

float BatteryStickCS3::voltage() {
    // M5.Power.getBatteryVoltage() returns mV.
    return M5.Power.getBatteryVoltage() / 1000.0f;
}

bool BatteryStickCS3::is_charging() {
    return M5.Power.isCharging();
}

}  // namespace hal
}  // namespace nocturnation
