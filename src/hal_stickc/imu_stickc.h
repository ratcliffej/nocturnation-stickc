// M5StickC Plus2 IMU backend.
//
// Wraps M5.Imu (MPU6886 on the Plus2: 6-axis accel + gyro).

#pragma once

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class IMUStickC : public IMU {
public:
    void begin() override;
    bool read(IMUSample& out) override;
};

}  // namespace hal
}  // namespace nocturnation
