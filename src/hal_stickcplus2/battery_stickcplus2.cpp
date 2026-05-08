#include "battery_stickcplus2.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

int BatteryStickCplus2::level_percent() {
    return M5.Power.getBatteryLevel();
}

float BatteryStickCplus2::voltage() {
    // M5.Power.getBatteryVoltage() returns mV.
    return M5.Power.getBatteryVoltage() / 1000.0f;
}

bool BatteryStickCplus2::is_charging() {
    return M5.Power.isCharging();
}

}  // namespace hal
}  // namespace nocturnation
