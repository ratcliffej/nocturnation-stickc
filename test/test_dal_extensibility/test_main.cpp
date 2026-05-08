// Native test for the Epic 2 extensibility AC:
//
//   "Adding a new device type requires only a new profile entry (and a new
//    driver if its protocol is new), with no changes to orchestration or
//    effects (verified by registering a stub TestDevice profile and
//    confirming fire_event against it works without orchestration code
//    changes)."
//
// This test defines a brand-new device profile and a brand-new transport
// driver entirely within this translation unit, registers them through the
// existing DAL public API, fires events at the new device, and asserts the
// driver received them with the right payload. Crucially, src/dal/, src/
// hal_stickc/, src/main.cpp, src/modes/ and the existing PixMob profile +
// driver are all untouched.
//
// Note on the AC wording: it says "JSON profile entry". Per the Epic 2
// design decision (project_hal_dal_architecture memory), JSON-loaded
// profiles were deferred; current pattern is C++ struct profiles. The
// substantive AC - "new device type requires no orchestration changes" -
// is what this test demonstrates.

#include <unity.h>
#include <cstring>
#include "hal/hal.h"
#include "dal/dal.h"

// =============================================================================
// Minimal HAL backend
// =============================================================================
//
// The DAL needs *some* HAL to compose its host profile from. This backend
// declares Display only, so the host profile gets only display outputs and
// nothing this test cares about colliding with.

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = { Capability::Display };
static constexpr size_t kCapabilityCount =
    sizeof(kCapabilities) / sizeof(kCapabilities[0]);

const Capability* HAL::capabilities()    { return kCapabilities; }
size_t            HAL::capability_count() { return kCapabilityCount; }
bool              HAL::has(Capability c) { return c == Capability::Display; }
void              HAL::begin()           {}
void              HAL::loop_tick()       {}

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

// =============================================================================
// Brand-new device type defined entirely in this test
// =============================================================================
//
// TestDevice declares two output capabilities (RgbStatic + RgbPulse) on a
// transport called "inert-test". It is a device type the DAL has never seen
// before, served by a driver the DAL has never seen before.

namespace test_device {

using namespace nocturnation::dal;

constexpr CapabilityId kOutputCaps[] = {
    CapabilityId::RgbStatic,
    CapabilityId::RgbPulse,
    CapabilityId::AssignDeviceGroup,
};

const DeviceProfile kProfile = DeviceProfile{
    /* type_id                 = */ "TestDevice",
    /* version                 = */ "1.0",
    /* transport               = */ "inert-test",
    /* output_capabilities     = */ kOutputCaps,
    /* output_capability_count = */ sizeof(kOutputCaps)/sizeof(kOutputCaps[0]),
    /* input_capabilities      = */ nullptr,
    /* input_capability_count  = */ 0,
    /* supports_groups         = */ true,
    /* max_group_id            = */ 7,
};

// =============================================================================
// Brand-new transport driver. Records every dispatched event so tests can
// assert what landed.
// =============================================================================

class TestDriver : public Driver {
public:
    const char* transport_name() const override { return "inert-test"; }
    bool        begin()                override { return true; }

    bool send(uint8_t group_id, const RgbStaticEvent& ev) override {
        last_static_  = ev;
        last_static_group_ = group_id;
        static_count_++;
        return true;
    }
    bool send(uint8_t group_id, const RgbPulseEvent& ev) override {
        last_pulse_   = ev;
        last_pulse_group_ = group_id;
        pulse_count_++;
        return true;
    }
    bool send(uint8_t /*group_id*/, const AssignDeviceGroupEvent& ev) override {
        last_assign_     = ev;
        assign_count_++;
        return true;
    }

    void reset() {
        static_count_ = 0;
        pulse_count_  = 0;
        assign_count_ = 0;
        last_static_  = RgbStaticEvent{};
        last_pulse_   = RgbPulseEvent{};
        last_assign_  = AssignDeviceGroupEvent{};
        last_static_group_ = 0xFF;
        last_pulse_group_  = 0xFF;
    }

    int                    static_count()      const { return static_count_; }
    int                    pulse_count()       const { return pulse_count_; }
    int                    assign_count()      const { return assign_count_; }
    RgbStaticEvent         last_static()       const { return last_static_; }
    RgbPulseEvent          last_pulse()        const { return last_pulse_; }
    AssignDeviceGroupEvent last_assign()       const { return last_assign_; }
    uint8_t                last_static_group() const { return last_static_group_; }
    uint8_t                last_pulse_group()  const { return last_pulse_group_; }

private:
    int                    static_count_      = 0;
    int                    pulse_count_       = 0;
    int                    assign_count_      = 0;
    RgbStaticEvent         last_static_       = {};
    RgbPulseEvent          last_pulse_        = {};
    AssignDeviceGroupEvent last_assign_       = {};
    uint8_t                last_static_group_ = 0xFF;
    uint8_t                last_pulse_group_  = 0xFF;
};

TestDriver driver;

}  // namespace test_device

// =============================================================================
// Tests
// =============================================================================

using namespace nocturnation;

void setUp(void) {
    test_device::driver.reset();
    dal::DAL::begin();
    // Register the new device + driver via the existing DAL public API.
    // Zero changes to src/dal/, src/main.cpp, or any other orchestration.
    dal::DAL::register_device("test-target", &test_device::kProfile, /*group=*/3);
    dal::DAL::register_driver(&test_device::driver);
}

void tearDown(void) {}

static void test_test_device_appears_in_active_registry(void) {
    TEST_ASSERT_TRUE(dal::DAL::has_device("test-target"));
    const auto* p = dal::DAL::profile_of("test-target");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("TestDevice", p->type_id);
}

static void test_test_device_supports_declared_capabilities(void) {
    using dal::CapabilityId;
    TEST_ASSERT_TRUE (dal::DAL::supports("test-target", CapabilityId::RgbStatic));
    TEST_ASSERT_TRUE (dal::DAL::supports("test-target", CapabilityId::RgbPulse));
    TEST_ASSERT_FALSE(dal::DAL::supports("test-target", CapabilityId::DisplayShowText));
    TEST_ASSERT_FALSE(dal::DAL::supports("test-target", CapabilityId::AudioFrame));
}

static void test_fire_rgb_static_lands_at_test_driver_with_payload(void) {
    dal::RgbStaticEvent ev{0x10, 0x20, 0x30};
    TEST_ASSERT_TRUE(dal::DAL::fire_rgb_static("test-target", ev));

    TEST_ASSERT_EQUAL_INT(1, test_device::driver.static_count());
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.pulse_count());

    auto received = test_device::driver.last_static();
    TEST_ASSERT_EQUAL_UINT8(0x10, received.r);
    TEST_ASSERT_EQUAL_UINT8(0x20, received.g);
    TEST_ASSERT_EQUAL_UINT8(0x30, received.b);

    // The test-target was registered with group_id=3; the driver should see it.
    TEST_ASSERT_EQUAL_UINT8(3, test_device::driver.last_static_group());
}

static void test_fire_rgb_pulse_lands_at_test_driver_with_payload(void) {
    dal::RgbPulseEvent ev{0xAA, 0xBB, 0xCC,
                          pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_192_MS,
                          pixmob::CHANCE_50};
    TEST_ASSERT_TRUE(dal::DAL::fire_rgb_pulse("test-target", ev));

    TEST_ASSERT_EQUAL_INT(1, test_device::driver.pulse_count());
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.static_count());

    auto received = test_device::driver.last_pulse();
    TEST_ASSERT_EQUAL_UINT8(0xAA, received.r);
    TEST_ASSERT_EQUAL_UINT8(0xBB, received.g);
    TEST_ASSERT_EQUAL_UINT8(0xCC, received.b);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_32_MS,  (int)received.attack);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_96_MS,  (int)received.sustain);
    TEST_ASSERT_EQUAL_INT((int)pixmob::T_192_MS, (int)received.release);
    TEST_ASSERT_EQUAL_INT((int)pixmob::CHANCE_50, (int)received.chance);

    TEST_ASSERT_EQUAL_UINT8(3, test_device::driver.last_pulse_group());
}

static void test_fire_unsupported_capability_silently_fails(void) {
    // TestDevice doesn't declare DisplayShowText, so dispatch must return
    // false without ever reaching the driver.
    dal::DisplayShowTextEvent ev{0, 0, "ignored", 0, 0, 1};
    TEST_ASSERT_FALSE(dal::DAL::fire_display_show_text("test-target", ev));
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.static_count());
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.pulse_count());
}

static void test_fire_to_unknown_device_silently_fails(void) {
    dal::RgbStaticEvent ev{0xFF, 0xFF, 0xFF};
    TEST_ASSERT_FALSE(dal::DAL::fire_rgb_static("ghost-target", ev));
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.static_count());
}

static void test_fire_assign_device_group_lands_at_test_driver(void) {
    dal::AssignDeviceGroupEvent ev{/*new_group_id=*/7};
    TEST_ASSERT_TRUE(dal::DAL::fire_assign_device_group("test-target", ev));

    TEST_ASSERT_EQUAL_INT(1, test_device::driver.assign_count());
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.pulse_count());
    TEST_ASSERT_EQUAL_UINT8(7, test_device::driver.last_assign().new_group_id);
}

static void test_pixmob_profile_declares_assign_device_group(void) {
    using dal::CapabilityId;
    // Pre-registered group-N devices share the PixMobX4Gen3_1 profile,
    // which now declares both RgbPulse and AssignDeviceGroup outputs.
    TEST_ASSERT_TRUE(dal::DAL::supports("group-3", CapabilityId::RgbPulse));
    TEST_ASSERT_TRUE(dal::DAL::supports("group-3", CapabilityId::AssignDeviceGroup));
    TEST_ASSERT_FALSE(dal::DAL::supports("group-3", CapabilityId::DisplayShowText));
}

static void test_group_n_devices_registered_after_begin(void) {
    // DAL::begin registers group-1..group-5 alongside local + all-pixmobs.
    // Each is bound to its corresponding group_id - this test cannot read
    // the group_id directly via the public API, but can confirm the named
    // device exists with the PixMob profile.
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-1"));
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-2"));
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-3"));
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-4"));
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-5"));
    TEST_ASSERT_FALSE(dal::DAL::has_device("group-6"));

    const auto* p = dal::DAL::profile_of("group-3");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("PixMobX4Gen3_1", p->type_id);
}

static void test_existing_pixmob_routing_unaffected(void) {
    // The DAL still has its baseline registrations ("local" + "all-pixmobs").
    // Firing to all-pixmobs should not touch the TestDriver. (No PixMobIRDriver
    // is registered in this test because the test HAL has no IRTx, so the
    // call returns false silently - the important assertion is that nothing
    // accidentally lands at the TestDriver.)
    dal::RgbPulseEvent ev{0xFF, 0x00, 0x00,
                          pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS,
                          pixmob::CHANCE_100};
    dal::DAL::fire_rgb_pulse("all-pixmobs", ev);
    TEST_ASSERT_EQUAL_INT(0, test_device::driver.pulse_count());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_test_device_appears_in_active_registry);
    RUN_TEST(test_test_device_supports_declared_capabilities);
    RUN_TEST(test_fire_rgb_static_lands_at_test_driver_with_payload);
    RUN_TEST(test_fire_rgb_pulse_lands_at_test_driver_with_payload);
    RUN_TEST(test_fire_unsupported_capability_silently_fails);
    RUN_TEST(test_fire_to_unknown_device_silently_fails);
    RUN_TEST(test_fire_assign_device_group_lands_at_test_driver);
    RUN_TEST(test_pixmob_profile_declares_assign_device_group);
    RUN_TEST(test_group_n_devices_registered_after_begin);
    RUN_TEST(test_existing_pixmob_routing_unaffected);
    return UNITY_END();
}
