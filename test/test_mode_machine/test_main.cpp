// Native test for the Mode FSM.
//
// Provides a miniature HAL backend (Mic + Buttons + Display) so DAL::begin()
// composes a host profile that lets ModeMachine subscribe to button + audio
// events. Drives the FSM by injecting button events via DAL::deliver_*()
// and advancing the test seam's millis() clock.

#include <unity.h>
#include <cstring>
#include "hal/hal.h"
#include "hal/input_action.h"
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

// DirectorMode migrated to InputAction-driven control in Block 10;
// the raw Btn2-LongPressed -> Menu handler is gone (the picker holds a
// "<- Menu" row that the operator reaches via Picker + Confirm). Tests
// that need to leave AutonomousMaster inject the semantic actions
// directly here. The other runtime modes still consume raw button events.
static void inject_input_action(hal::InputAction action) {
    dal::DAL::deliver_input_action("local",
        hal::InputEvent{action, /*timestamp_ms=*/0});
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
    TEST_ASSERT_EQUAL_INT((int)ModeId::Director,
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
    TEST_ASSERT_EQUAL_INT((int)ModeId::Director,
                          (int)ModeMachine::current());
}

static void test_menu_btn2_cycles_then_btn1_selects_slave(void) {
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // Boot -> Menu
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // Menu order is Master, Slave, Test, Config - so one Btn2 press lands on Slave.
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Lume, (int)ModeMachine::current());
}

static void test_menu_cycle_wraps(void) {
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);  // Boot -> Menu

    // Four Btn2 presses with a 4-item menu cycles back to start; Btn1 then
    // selects AutonomousMaster (the first item).
    for (int i = 0; i < 4; ++i) {
        inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    }
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Director,
                          (int)ModeMachine::current());
}

static void test_long_press_btnb_returns_to_menu_from_each_runtime_mode(void) {
    // From AutonomousMaster (Block 10): InputAction::Picker opens the
    // picker overlay; with no vis registered in this test env the
    // picker contains only the "<- Menu" sentinel at cursor=0, so a
    // straight Confirm switches to Menu.
    ModeMachine::switch_to(ModeId::Director);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Director,
                          (int)ModeMachine::current());
    inject_input_action(hal::InputAction::Picker);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());

    // From Slave
    ModeMachine::switch_to(ModeId::Lume);
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

// Config top-level menu shape (post-Epic-4.65 restructure):
//   0: Group       (direct action, A increments slv_group)
//   1: Display     (drill -> leaf)
//   2: Connectivity (drill -> picker -> IR / ESP-NOW / WiFi / DMX)
//   3: Utilities   (drill -> picker -> PixMob)
//   4: System      (drill -> leaf)
//
// Three navigation levels (Top / Picker / Sub) - the picker layer is
// new in this restructure. The B-hold contract:
//   Sub via picker -> Picker
//   Sub from Top   -> Top
//   Picker         -> Top
//   Top            -> Menu (exits Config)
//
// This test exercises the deepest path (Top -> Picker -> Sub) and
// asserts the picker-layer pop: from a leaf reached via Connectivity,
// the first B-hold must NOT exit Config (it should land on the picker),
// the second B-hold must NOT exit Config (it should land on Top), and
// only the third B-hold finally exits to Menu.
static void test_config_picker_layer_bhold_pops_one_level(void) {
    ModeMachine::switch_to(ModeId::Config);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());

    // Top cursor starts at 0 (Group). Three Btn2 presses cycles to
    // Connectivity (index 3 after Epic 4.7 Block 1 inserted Show at
    // index 1: Group / Show / Display / Connectivity / Utilities /
    // System).
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    // Btn1 drills into the Connectivity picker.
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());
    // Btn1 again drills into the first picker entry (IR).
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());

    // Three B-holds to climb out: Sub -> Picker -> Top -> Menu.
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());
}

// Display is a direct-drill leaf submenu (not behind a picker). B-hold
// from Display must skip the picker layer and pop straight to Top, so
// only two B-holds total are needed to escape Config.
static void test_config_direct_leaf_bhold_skips_picker(void) {
    ModeMachine::switch_to(ModeId::Config);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());

    // Top cursor at 0 (Group); two Btn2 presses land on Display
    // (index 2 after Epic 4.7 Block 1 inserted Show at index 1).
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::Pressed);
    inject_button_press(hal::ButtonId::Btn1, hal::ButtonEvent::Pressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());

    // Two B-holds to climb out: Sub -> Top -> Menu (no picker layer).
    inject_button_press(hal::ButtonId::Btn2, hal::ButtonEvent::LongPressed);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Config, (int)ModeMachine::current());
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
    RUN_TEST(test_config_picker_layer_bhold_pops_one_level);
    RUN_TEST(test_config_direct_leaf_bhold_skips_picker);
    RUN_TEST(test_test_mode_short_press_does_not_leave_mode);
    return UNITY_END();
}
