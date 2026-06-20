// Native test: HAL capability declaration + query mechanism.
//
// Verifies the contract from the HAL design notes §2 (nocturnation-docs repo) - that a backend
// declaring a known capability set surfaces the right answers from
// HAL::has(), HAL::capabilities(), and HAL::capability_count(), and that
// accessors for un-declared capabilities return nullptr.
//
// This test ships its own miniature HAL backend declaring three capabilities
// (Mic, IRTx, Display); the StickC Plus2 backend in src/hal_stickcplus2/ is excluded
// from the native build (build_src_filter = -<*>) so there's no symbol
// collision.

#include <unity.h>
#include "hal/hal.h"

// -----------------------------------------------------------------------------
// Test backend: declares Mic, IRTx, Display; omits the rest
// -----------------------------------------------------------------------------

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = {
    Capability::Mic,
    Capability::IRTx,
    Capability::Display,
};
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()     { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }

bool HAL::has(Capability c) {
    for (size_t i = 0; i < kCapabilityCount; ++i) {
        if (kCapabilities[i] == c) return true;
    }
    return false;
}

void HAL::begin()     {}
void HAL::loop_tick() {}

Mic*     HAL::mic()      { return nullptr; }
IRTx*    HAL::ir_tx()    { return nullptr; }
IRTx*    HAL::ir_tx_ext() { return nullptr; }
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return nullptr; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------

void setUp(void) {}
void tearDown(void) {}

static void test_capability_count(void) {
    using namespace nocturnation::hal;
    TEST_ASSERT_EQUAL_size_t(3, HAL::capability_count());
}

static void test_declared_capabilities_present(void) {
    using namespace nocturnation::hal;
    TEST_ASSERT_TRUE_MESSAGE(HAL::has(Capability::Mic),     "Mic should be declared");
    TEST_ASSERT_TRUE_MESSAGE(HAL::has(Capability::IRTx),    "IRTx should be declared");
    TEST_ASSERT_TRUE_MESSAGE(HAL::has(Capability::Display), "Display should be declared");
}

static void test_undeclared_capabilities_absent(void) {
    using namespace nocturnation::hal;
    TEST_ASSERT_FALSE_MESSAGE(HAL::has(Capability::IRRx),    "IRRx should NOT be declared");
    TEST_ASSERT_FALSE_MESSAGE(HAL::has(Capability::ESPNow),  "ESPNow should NOT be declared");
    TEST_ASSERT_FALSE_MESSAGE(HAL::has(Capability::Buttons), "Buttons should NOT be declared");
    TEST_ASSERT_FALSE_MESSAGE(HAL::has(Capability::IMU),     "IMU should NOT be declared");
    TEST_ASSERT_FALSE_MESSAGE(HAL::has(Capability::Battery), "Battery should NOT be declared");
}

static void test_accessors_return_nullptr_for_undeclared(void) {
    using namespace nocturnation::hal;
    // All accessors return nullptr in this stub; in a real backend the
    // declared ones return concrete instances. The undeclared-returns-null
    // contract is the load-bearing one.
    TEST_ASSERT_NULL(HAL::ir_rx());
    TEST_ASSERT_NULL(HAL::esp_now());
    TEST_ASSERT_NULL(HAL::buttons());
    TEST_ASSERT_NULL(HAL::imu());
    TEST_ASSERT_NULL(HAL::battery());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_capability_count);
    RUN_TEST(test_declared_capabilities_present);
    RUN_TEST(test_undeclared_capabilities_absent);
    RUN_TEST(test_accessors_return_nullptr_for_undeclared);
    return UNITY_END();
}
