// Native test for the Mode FSM.
//
// Provides a miniature HAL backend (Mic + Buttons + Display) so DAL::begin()
// composes a host profile that lets ModeMachine subscribe to button + audio
// events. Drives the FSM by injecting button events via DAL::deliver_*()
// and advancing the test seam's millis() clock.

#include <unity.h>
#include <cstring>
#include "hal/hal.h"
#include "dal/dal.h"
#include "modes/mode_machine.h"

// =============================================================================
// Test HAL backend
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

const Capability* HAL::capabilities()    { return kCapabilities; }
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
using modes::ModeId;
using modes::ModeMachine;
using modes::test_seam::set_millis;

static void inject_button_press(hal::ButtonId id, hal::ButtonEvent kind) {
    dal::DAL::deliver_button_press("local", dal::ButtonPressEvent{id, kind});
}

void setUp(void) {
    // Reset the millis() seam first so BootMode::enter() captures start_ms_=0.
    set_millis(0);
    dal::DAL::begin();          // resets registries; subscribes nothing
    ModeMachine::begin();       // subscribes to buttons + audio, enters Boot
}

void tearDown(void) {}

static void test_starts_in_boot_mode(void) {
    TEST_ASSERT_EQUAL_INT((int)ModeId::Boot, (int)ModeMachine::current());
    TEST_ASSERT_EQUAL_STRING("Boot", ModeMachine::current_name());
}

static void test_boot_timeout_enters_default_runtime_mode(void) {
    // No NVS in native builds, so the persisted-mode loader returns the
    // hard-coded default (AutonomousMaster). After 5s of countdown,
    // BootMode::loop_tick() should switch to it.
    set_millis(5001);
    ModeMachine::loop_tick();
    TEST_ASSERT_EQUAL_INT((int)ModeId::AutonomousMaster,
                          (int)ModeMachine::current());
}

static void test_boot_button_press_enters_menu(void) {
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());
}

static void test_menu_btn1_selects_default_first_item(void) {
    // Default cursor lands on the persisted last-used mode, which on first
    // boot is AutonomousMaster - that's also the first menu item, so a
    // straight Btn1 selects AutonomousMaster.
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // Boot -> Menu
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // select
    TEST_ASSERT_EQUAL_INT((int)ModeId::AutonomousMaster,
                          (int)ModeMachine::current());
}

static void test_menu_btn2_cycles_then_btn1_selects_slave(void) {
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // Boot -> Menu
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // Menu order is Master, Slave, Test, Config - so one Btn2 press lands on Slave.
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Slave, (int)ModeMachine::current());
}

static void test_menu_cycle_wraps(void) {
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // Boot -> Menu

    // Four Btn2 presses with a 4-item menu cycles back to start; Btn1 then
    // selects AutonomousMaster (the first item).
    for (int i = 0; i < 4; ++i) {
        inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    }
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::AutonomousMaster,
                          (int)ModeMachine::current());
}

static void test_long_press_btnb_returns_to_menu_from_each_runtime_mode(void) {
    // From AutonomousMaster
    ModeMachine::switch_to(ModeId::AutonomousMaster);
    TEST_ASSERT_EQUAL_INT((int)ModeId::AutonomousMaster,
                          (int)ModeMachine::current());
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // From Slave
    ModeMachine::switch_to(ModeId::Slave);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // From Config
    ModeMachine::switch_to(ModeId::Config);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // From Test
    ModeMachine::switch_to(ModeId::Test);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());
}

static void test_test_mode_short_press_does_not_leave_mode(void) {
    // In Test mode, Btn1 should fire IR (no driver registered, so it's a
    // silent no-op) but must not transition out of Test.
    ModeMachine::switch_to(ModeId::Test);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Test, (int)ModeMachine::current());

    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Test, (int)ModeMachine::current());

    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Test, (int)ModeMachine::current());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_in_boot_mode);
    RUN_TEST(test_boot_timeout_enters_default_runtime_mode);
    RUN_TEST(test_boot_button_press_enters_menu);
    RUN_TEST(test_menu_btn1_selects_default_first_item);
    RUN_TEST(test_menu_btn2_cycles_then_btn1_selects_slave);
    RUN_TEST(test_menu_cycle_wraps);
    RUN_TEST(test_long_press_btnb_returns_to_menu_from_each_runtime_mode);
    RUN_TEST(test_test_mode_short_press_does_not_leave_mode);
    return UNITY_END();
}
