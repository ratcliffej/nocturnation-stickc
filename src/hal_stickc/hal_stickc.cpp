// NocturNation HAL - M5StickC Plus2 backend.
//
// Epic 2 starting point: the contract is in place but no capabilities are
// declared yet. As each capability gets a real implementation, it gets added
// to the `kCapabilities` array and its accessor below starts returning a
// concrete instance.
//
// While this stub declares no capabilities, main.cpp continues to use
// M5Unified directly (per Epic 1's behaviour-preserving baseline). Each
// capability migration is its own incremental commit + hardware verification.

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

// -----------------------------------------------------------------------------
// Capability list (empty - filled in as backends land for each capability)
// -----------------------------------------------------------------------------

static constexpr Capability kCapabilities[] = {};
static constexpr size_t     kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()    { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }

bool HAL::has(Capability c) {
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (kCapabilities[i] == c) return true;
    }
    return false;
}

// -----------------------------------------------------------------------------
// Lifecycle
// -----------------------------------------------------------------------------

void HAL::begin()     {}
void HAL::loop_tick() {}

// -----------------------------------------------------------------------------
// Accessors - all nullptr until the corresponding implementation lands
// -----------------------------------------------------------------------------

Mic*     HAL::mic()      { return nullptr; }
IRTx*    HAL::ir_tx()    { return nullptr; }
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return nullptr; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation
