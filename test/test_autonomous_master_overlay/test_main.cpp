// Native test: DirectorMode picker + settings overlays
// (Epic 4.7 Block 1).
//
// Mode now hosts a Show (Epic 4.7) instead of a Visualisation
// (pre-Block-1). The picker enumerates show_registry; Settings
// auto-generates UI from active_show->properties(). Tests verify the
// overlay state machine, the picker's NVS persistence (active_show
// key), the settings enum advance / wrap / back semantics, and the
// resolver fallback when the saved id no longer resolves.
//
// Tests that compared two registered Shows (gate flip, switch-between)
// are intentionally dropped this block: only SimpleBeatShow is
// registered in Block 1. Block 5 (DynamicShow) restores multi-show
// coverage.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "hal/input_action.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "shows/show_registry.h"
#include "shows/simple_beat_show.h"
#include "modes/mode_machine.h"

// DirectorMode is a private header inside src/modes/. The test
// build's include path adds src/modes/ so we can reach the test-seam
// accessors directly without exposing them in the public include/ tree.
#include "../../src/modes/director_mode.h"
#include "../../src/modes/persistence.h"

// =============================================================================
// Native millis() seam (matches mode_machine.cpp's pattern)
// =============================================================================

using namespace nocturnation;
using modes::ModeId;
using modes::ModeMachine;
using modes::test_seam::set_millis;
using modes::DirectorMode;
using shows::show_registry;
using shows::simple_beat_show_instance;
using plugins::PropertyBag;
using plugins::PropertyValue;

// =============================================================================
// Test HAL backend - Mic + Display + Buttons + AnalyserBeatDetection.
// =============================================================================

namespace nocturnation {
namespace hal {

class StubDisplay : public Display {
public:
    void begin() override {}
    void set_rotation(uint8_t) override {}
    int  width()  const override { return 240; }
    int  height() const override { return 135; }
    void clear(uint16_t) override {}
    void fill_rect(int, int, int, int, uint16_t) override {}
    void draw_rect(int, int, int, int, uint16_t) override {}
    void draw_hline(int, int, int, uint16_t) override {}
    void draw_vline(int, int, int, uint16_t) override {}
    void set_text_color(uint16_t, uint16_t) override {}
    void set_text_size(uint8_t) override {}
    void draw_text(int, int, const char*) override {}
    void flush() override {}
    bool begin_buffered_paint(int, int, int, int) override { return false; }
    void end_buffered_paint() override {}
};
static StubDisplay s_stub_display;

static constexpr Capability kCapabilities[] = {
    Capability::Mic,
    Capability::Display,
    Capability::Buttons,
    Capability::AnalyserBeatDetection,
    Capability::AnalyserSpectrumFrame,
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
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }
LedStrip* HAL::led_strip() { return nullptr; }
uint8_t HAL::max_strip_brightness_percent() { return 100; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Test helpers
// =============================================================================

static DirectorMode* master_instance() {
    extern DirectorMode& test_get_director();
    return &test_get_director();
}

static void inject_input_action(hal::InputAction action) {
    dal::DAL::deliver_input_action("local",
        hal::InputEvent{action, /*timestamp_ms=*/0});
}

// =============================================================================
// Unity setup / teardown
// =============================================================================

void setUp(void) {
    set_millis(0);
    PropertyBag::clear_for_tests();
    show_registry().clear();
    modes::persistence::test_seam::clear_native_persistence();
    // Register the canonical SimpleBeatShow - matches main.cpp's startup
    // wiring. Block 5 (DynamicShow) will register a second Show; tests
    // covering multi-show behaviour return then.
    show_registry().register_plugin(simple_beat_show_instance());

    dal::DAL::begin();
    ModeMachine::begin();
    // Skip past the Boot countdown.
    set_millis(6000);
    ModeMachine::loop_tick();
    ModeMachine::switch_to(ModeId::Director);
}

void tearDown(void) {}

// =============================================================================
// Picker overlay tests
// =============================================================================

static void test_picker_opens_and_closes_on_picker_action(void) {
    auto* m = master_instance();
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::None,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::Picker,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

static void test_picker_cycle_advances_cursor_and_wraps(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_size_t(0u, m->overlay_cursor_for_tests());

    // Registry has 1 show ("simple-beat"); picker_row_count = 2
    // (1 show + "<- Menu" sentinel). Cycle wraps after the second row.
    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(1u, m->overlay_cursor_for_tests());

    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(0u, m->overlay_cursor_for_tests());   // wrap
}

static void test_picker_confirm_show_row_persists_selection(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Picker);
    // Cursor=0 -> "simple-beat". Confirm should keep us in
    // Director and persist "simple-beat" to NVS.
    inject_input_action(hal::InputAction::Confirm);

    TEST_ASSERT_EQUAL_INT((int)ModeId::Director,
                          (int)ModeMachine::current());
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::None,
        (int)m->overlay_for_tests());
    TEST_ASSERT_EQUAL_STRING("simple-beat",
        modes::persistence::load_active_show_id());
}

static void test_picker_back_menu_row_switches_to_menu(void) {
    inject_input_action(hal::InputAction::Picker);
    // Cycle once to land on "<- Menu" (row index 1 with one show).
    inject_input_action(hal::InputAction::Cycle);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());
}

// =============================================================================
// Status-strip label
// =============================================================================

static void test_status_label_reflects_active_show_display_name(void) {
    auto* m = master_instance();
    const char* label = m->status_label_for_tests();
    TEST_ASSERT_NOT_NULL(label);
    TEST_ASSERT_NOT_EQUAL('\0', label[0]);
    TEST_ASSERT_EQUAL_STRING(simple_beat_show_instance()->display_name(),
                              label);
}

// =============================================================================
// Settings overlay
// =============================================================================

static void test_settings_opens_and_closes_on_settings_action(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Settings);
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::Settings,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Settings);
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

static void test_settings_confirm_on_enum_advances_value(void) {
    // SimpleBeatShow's "color" property is the only schema entry:
    // Enum [0..5], default 1 (Red). Confirm on the first property row
    // should advance to 2 (Green).
    auto& bag = shows::simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(1));

    inject_input_action(hal::InputAction::Settings);
    inject_input_action(hal::InputAction::Confirm);   // edit row 0

    TEST_ASSERT_EQUAL_UINT8(2, bag.get("color").as_enum());
}

static void test_settings_confirm_on_enum_wraps_at_max(void) {
    auto& bag = shows::simple_beat_show_property_bag();
    bag.set("color", PropertyValue::from_enum(5));    // max
    inject_input_action(hal::InputAction::Settings);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_UINT8(0, bag.get("color").as_enum()); // wrap to min
}

static void test_settings_back_row_closes_overlay(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Settings);
    // SimpleBeatShow has 1 property; back row sits at cursor=1.
    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(1u, m->overlay_cursor_for_tests());
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_INT(
        (int)DirectorMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

// =============================================================================
// NVS fallback - saved id that doesn't resolve falls back to simple-beat.
// =============================================================================

static void test_unknown_saved_id_falls_back_to_simple_beat(void) {
    modes::persistence::save_active_show_id("nonsense-id");
    ModeMachine::switch_to(ModeId::Menu);
    ModeMachine::switch_to(ModeId::Director);

    auto* m = master_instance();
    TEST_ASSERT_EQUAL_STRING("simple-beat", m->active_show_id_for_tests());
}

// =============================================================================
// Persisted picker selection survives a mode round-trip.
// =============================================================================

static void test_persisted_picker_selection_survives_round_trip(void) {
    inject_input_action(hal::InputAction::Picker);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_STRING("simple-beat",
        modes::persistence::load_active_show_id());

    ModeMachine::switch_to(ModeId::Menu);
    ModeMachine::switch_to(ModeId::Director);
    auto* m = master_instance();
    TEST_ASSERT_EQUAL_STRING("simple-beat", m->active_show_id_for_tests());
}

// =============================================================================
// Unity main
// =============================================================================

int main(int /*argc*/, char** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_picker_opens_and_closes_on_picker_action);
    RUN_TEST(test_picker_cycle_advances_cursor_and_wraps);
    RUN_TEST(test_picker_confirm_show_row_persists_selection);
    RUN_TEST(test_picker_back_menu_row_switches_to_menu);
    RUN_TEST(test_status_label_reflects_active_show_display_name);
    RUN_TEST(test_settings_opens_and_closes_on_settings_action);
    RUN_TEST(test_settings_confirm_on_enum_advances_value);
    RUN_TEST(test_settings_confirm_on_enum_wraps_at_max);
    RUN_TEST(test_settings_back_row_closes_overlay);
    RUN_TEST(test_unknown_saved_id_falls_back_to_simple_beat);
    RUN_TEST(test_persisted_picker_selection_survives_round_trip);
    return UNITY_END();
}
