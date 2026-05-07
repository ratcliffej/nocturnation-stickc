// Native test for the DAL: registry, capability queries, fire-when-no-driver
// (silent-fail), subscription wiring, and event delivery to subscribers.
//
// Provides its own miniature HAL backend so the DAL has something to compose
// the host profile from. The StickC HAL backend in src/hal_stickc/ is excluded
// from the native build (build_src_filter); the DAL's own src/dal/dal.cpp is
// included via "+<dal/>". No drivers register here - the test exercises the
// dispatch routing rather than any specific protocol behaviour.

#include <unity.h>
#include "hal/hal.h"
#include "dal/dal.h"

// =============================================================================
// Test HAL backend - declares Mic + Buttons + Display (no IRTx, no ESPNow)
// so the composed host profile gets AudioFrame + ButtonPress as inputs and
// the four display capabilities as outputs.
// =============================================================================

namespace nocturnation {
namespace hal {

static constexpr Capability kCapabilities[] = {
    Capability::Mic,
    Capability::Buttons,
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
IRRx*    HAL::ir_rx()    { return nullptr; }
ESPNow*  HAL::esp_now()  { return nullptr; }
Display* HAL::display()  { return nullptr; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Tests
// =============================================================================

using namespace nocturnation;

void setUp(void) {
    // DAL::begin() is idempotent - it resets internal registries before
    // populating them - so each test starts from a known state.
    dal::DAL::begin();
}

void tearDown(void) {}

static void test_local_device_registered_after_begin(void) {
    TEST_ASSERT_TRUE(dal::DAL::has_device("local"));
    TEST_ASSERT_NOT_NULL(dal::DAL::profile_of("local"));
}

static void test_local_profile_reflects_hal_capabilities(void) {
    using dal::CapabilityId;
    // HAL has Mic + Buttons + Display, so the composed host profile should
    // declare AudioFrame + ButtonPress as inputs and the four display
    // capabilities as outputs. ESPNow is NOT declared, so EspNowInbound
    // must NOT appear as an input.
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::AudioFrame));
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::ButtonPress));
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::DisplayShowText));
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::DisplayClear));
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::DisplayFillRect));
    TEST_ASSERT_TRUE (dal::DAL::supports("local", CapabilityId::DisplayMeter));
    TEST_ASSERT_FALSE(dal::DAL::supports("local", CapabilityId::EspNowInbound));
    TEST_ASSERT_FALSE(dal::DAL::supports("local", CapabilityId::DmxInbound));
    TEST_ASSERT_FALSE(dal::DAL::supports("local", CapabilityId::RgbPulse));
}

static void test_register_pixmob_device(void) {
    TEST_ASSERT_TRUE(dal::DAL::register_device("group-5",
                                               &dal::profiles::PixMobX4Gen3_1, 5));
    TEST_ASSERT_TRUE(dal::DAL::has_device("group-5"));
    TEST_ASSERT_TRUE(dal::DAL::supports("group-5", dal::CapabilityId::RgbPulse));
    TEST_ASSERT_FALSE(dal::DAL::supports("group-5", dal::CapabilityId::AudioFrame));
}

static void test_duplicate_registration_rejected(void) {
    TEST_ASSERT_TRUE(dal::DAL::register_device("dup",
                                               &dal::profiles::PixMobX4Gen3_1, 1));
    TEST_ASSERT_FALSE(dal::DAL::register_device("dup",
                                                &dal::profiles::PixMobX4Gen3_1, 1));
}

static void test_unknown_device_queries(void) {
    TEST_ASSERT_FALSE(dal::DAL::has_device("nope"));
    TEST_ASSERT_NULL (dal::DAL::profile_of("nope"));
    TEST_ASSERT_FALSE(dal::DAL::supports  ("nope", dal::CapabilityId::RgbPulse));
}

static void test_fire_returns_false_when_no_driver_registered(void) {
    // No driver is registered for the "ir-pixmob" transport in this test.
    // PixMob device's profile declares RgbPulse, so the capability check
    // passes; dispatch then fails because there's no driver. This is the
    // silent-fail path callers see when a transport prerequisite (here,
    // hal::IRTx + a registered PixMobIRDriver) is absent.
    dal::DAL::register_device("group-5", &dal::profiles::PixMobX4Gen3_1, 5);
    dal::RgbPulseEvent ev{255, 0, 0, pixmob::T_32_MS, pixmob::T_96_MS,
                          pixmob::T_96_MS, pixmob::CHANCE_100};
    TEST_ASSERT_FALSE(dal::DAL::fire_rgb_pulse("group-5", ev));
}

static void test_fire_returns_false_for_unknown_target(void) {
    dal::RgbPulseEvent ev{};
    TEST_ASSERT_FALSE(dal::DAL::fire_rgb_pulse("ghost-bracelet", ev));
}

static void test_fire_returns_false_for_unsupported_capability(void) {
    // PixMob doesn't have the DisplayShowText output capability, so even
    // if a driver existed for "ir-pixmob", this dispatch would fail.
    dal::DAL::register_device("group-5", &dal::profiles::PixMobX4Gen3_1, 5);
    dal::DisplayShowTextEvent ev{0, 0, "hello", 0xFFFF, 0x0000, 1};
    TEST_ASSERT_FALSE(dal::DAL::fire_display_show_text("group-5", ev));
}

static void test_subscribe_to_supported_input(void) {
    auto cb = [](const char*, const dal::AudioFrameEvent&) {};
    TEST_ASSERT_TRUE(dal::DAL::subscribe_audio_frames("local", cb));
}

static void test_subscribe_to_unsupported_input_fails(void) {
    // The "local" profile in this HAL config has no EspNow input
    // (HAL didn't declare ESPNow), so this subscription must fail silently.
    auto cb = [](const char*, const dal::EspNowInboundEvent&) {};
    TEST_ASSERT_FALSE(dal::DAL::subscribe_esp_now_inbound("local", cb));
}

static void test_subscribe_unknown_target_fails(void) {
    auto cb = [](const char*, const dal::AudioFrameEvent&) {};
    TEST_ASSERT_FALSE(dal::DAL::subscribe_audio_frames("ghost", cb));
}

static void test_audio_frame_delivered_to_subscriber(void) {
    int   call_count          = 0;
    float observed_bass       = -1.0f;
    auto  cb = [&](const char* source, const dal::AudioFrameEvent& ev) {
        if (source && std::string{source} == "local") {
            call_count++;
            observed_bass = ev.bass_energy;
        }
    };
    TEST_ASSERT_TRUE(dal::DAL::subscribe_audio_frames("local", cb));

    dal::AudioFrameEvent ev{42, 1234.5f, 678.9f, 99.0f, 250.0f};
    dal::DAL::deliver_audio_frame("local", ev);

    TEST_ASSERT_EQUAL_INT(1, call_count);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 1234.5f, observed_bass);
}

static void test_button_press_delivered_to_subscriber(void) {
    int call_count     = 0;
    auto cb = [&](const char*, const dal::ButtonPressEvent&) { call_count++; };
    TEST_ASSERT_TRUE(dal::DAL::subscribe_button_presses("local", cb));

    dal::ButtonPressEvent ev{hal::ButtonId::Btn1, hal::ButtonEvent::Clicked};
    dal::DAL::deliver_button_press("local", ev);

    TEST_ASSERT_EQUAL_INT(1, call_count);
}

static void test_active_device_listing(void) {
    dal::DAL::register_device("a", &dal::profiles::PixMobX4Gen3_1, 1);
    dal::DAL::register_device("b", &dal::profiles::PixMobX4Gen3_1, 2);
    // DAL::begin() registers "local" then "all-pixmobs"; this test then
    // adds "a" + "b". Total = 4, in registration order.
    TEST_ASSERT_EQUAL_size_t(4, dal::DAL::active_device_count());
    TEST_ASSERT_EQUAL_STRING("local",       dal::DAL::active_device_name(0));
    TEST_ASSERT_EQUAL_STRING("all-pixmobs", dal::DAL::active_device_name(1));
    TEST_ASSERT_EQUAL_STRING("a",           dal::DAL::active_device_name(2));
    TEST_ASSERT_EQUAL_STRING("b",           dal::DAL::active_device_name(3));
    TEST_ASSERT_NULL(dal::DAL::active_device_name(4));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_local_device_registered_after_begin);
    RUN_TEST(test_local_profile_reflects_hal_capabilities);
    RUN_TEST(test_register_pixmob_device);
    RUN_TEST(test_duplicate_registration_rejected);
    RUN_TEST(test_unknown_device_queries);
    RUN_TEST(test_fire_returns_false_when_no_driver_registered);
    RUN_TEST(test_fire_returns_false_for_unknown_target);
    RUN_TEST(test_fire_returns_false_for_unsupported_capability);
    RUN_TEST(test_subscribe_to_supported_input);
    RUN_TEST(test_subscribe_to_unsupported_input_fails);
    RUN_TEST(test_subscribe_unknown_target_fails);
    RUN_TEST(test_audio_frame_delivered_to_subscriber);
    RUN_TEST(test_button_press_delivered_to_subscriber);
    RUN_TEST(test_active_device_listing);
    return UNITY_END();
}
