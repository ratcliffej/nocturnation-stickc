#include "battery_stickc.h"
#include "M5Unified.h"

namespace nocturnation {
namespace hal {

int BatteryStickC::level_percent() {
    return M5.Power.getBatteryLevel();
}

float BatteryStickC::voltage() {
    // M5.Power.getBatteryVoltage() returns mV.
    return M5.Power.getBatteryVoltage() / 1000.0f;
}

bool BatteryStickC::is_charging() {
    return M5.Power.isCharging();
}

}  // namespace hal
}  // namespace nocturnation
