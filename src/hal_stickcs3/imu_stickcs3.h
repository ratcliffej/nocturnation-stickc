// M5StickS3 IMU backend.
//
// Wraps M5.Imu (BMI270 on the StickS3: 6-axis accel + gyro). M5Unified's
// query surface is identical to the Plus2's MPU6886 path, so behaviour
// mirrors IMUStickCplus2.

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class IMUStickCS3 : public IMU {
public:
    void begin() override;
    bool read(IMUSample& out) override;
};

}  // namespace hal
}  // namespace nocturnation
