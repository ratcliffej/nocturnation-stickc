#include "imu_stickcs3.h"
#include "M5Unified.h"
#include <Arduino.h>

namespace nocturnation {
namespace hal {

void IMUStickCS3::begin() {
    // M5.begin() initialises the IMU (BMI270 on the StickS3); nothing else needed.
}

bool IMUStickCS3::read(IMUSample& out) {
    float ax = 0, ay = 0, az = 0;
    float gx = 0, gy = 0, gz = 0;
    const bool a_ok = M5.Imu.getAccel(&ax, &ay, &az);
    const bool g_ok = M5.Imu.getGyro (&gx, &gy, &gz);
    if (!a_ok && !g_ok) return false;

    out.timestamp_ms = millis();
    out.ax = ax; out.ay = ay; out.az = az;
    out.gx = gx; out.gy = gy; out.gz = gz;
    out.has_gyro = g_ok;
    return true;
}

}  // namespace hal
}  // namespace nocturnation
