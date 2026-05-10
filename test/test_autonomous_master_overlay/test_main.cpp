// Native test: AutonomousMasterMode picker + settings overlays
// (Epic 4.6 Block 10).
//
// Provides a miniature HAL backend (Mic + Display + Buttons +
// AnalyserBeatDetection) so DAL::begin() composes a profile that
// satisfies BeatPulse's capability gate, then drives the overlay state
// machine through DAL::deliver_input_action.
//
// The tests rely on the test_seam accessors added in Block 10's
// AutonomousMasterMode header (overlay_for_tests / status_label_for_tests)
// rather than scraping the rendered framebuffer.

#include <unity.h>
#include <cstring>

#include "hal/hal.h"
#include "hal/input_action.h"
#include "dal/dal.h"
#include "plugins/plugin.h"
#include "plugins/property_bag.h"
#include "visualisations/visualisation.h"
#include "visualisations/visualisation_context.h"
#include "visualisations/visualisation_registry.h"
#include "visualisations/beat_pulse.h"
#include "modes/mode_machine.h"

// AutonomousMasterMode is a private header inside src/modes/. The test
// build's include path adds src/modes/ so we can reach the test-seam
// accessors directly without exposing them in the public include/ tree.
#include "../../src/modes/autonomous_master_mode.h"
#include "../../src/modes/persistence.h"

// =============================================================================
// Native millis() seam (strong definition; mode_machine.cpp provides a
// matching seam under #ifndef ARDUINO via test_seam::set_millis - this
// TU re-uses that)
// =============================================================================

using namespace nocturnation;
using modes::ModeId;
using modes::ModeMachine;
using modes::test_seam::set_millis;
using modes::AutonomousMasterMode;
using visualisations::visualisation_registry;
using visualisations::beat_pulse_instance;
using plugins::PropertyBag;
using plugins::PropertyValue;

// =============================================================================
// Test HAL backend - Mic + Display + Buttons + AnalyserBeatDetection.
// =============================================================================
//
// Buttons capability is declared so DAL composes a profile that lets
// ModeMachine subscribe to input actions on "local". HAL::buttons()
// still returns nullptr (we do NOT want the LocalDriver's input mapper
// chain to wire up; tests inject InputActions directly via
// deliver_input_action and bypass the mapper).

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
Display* HAL::display()  { return &s_stub_display; }
Buttons* HAL::buttons()  { return nullptr; }
IMU*     HAL::imu()      { return nullptr; }
Battery* HAL::battery()  { return nullptr; }

}  // namespace hal
}  // namespace nocturnation

// =============================================================================
// Test helpers
// =============================================================================

static AutonomousMasterMode* master_instance() {
    // The FSM owns one static instance of each mode; reach it via the
    // public ModeMachine::switch_to + a downcast. dynamic_cast is not
    // available in -fno-rtti builds so we walk through the typed ptr
    // by switching, asserting current(), and using a static helper.
    // The cleanest path is to call switch_to(AutonomousMaster) and
    // then access the singleton via a thin TU-local forward.
    //
    // For the overlay tests we add a tiny accessor inside this TU that
    // walks ModeMachine: switch to AutonomousMaster, and trust that
    // current() is the master pointer. Then we cast through a tag.
    //
    // Simpler approach: declare a forward to mode_machine's symbol -
    // mode_machine.cpp keeps these as file-static. So we'd need a new
    // accessor. Instead, we reach the mode through the public surface:
    // there is no need for a pointer; we just inject events and read
    // back via the testing seam exposed on the class. But the seam
    // is a member method, so we need a pointer.
    //
    // Path of least resistance: extern a helper from mode_machine.cpp.
    // mode_machine.cpp declares its s_autonomous_master in a private
    // anonymous namespace. Adding an accessor there is the cleanest.
    extern AutonomousMasterMode& test_get_autonomous_master();
    return &test_get_autonomous_master();
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
    visualisation_registry().clear();
    modes::persistence::test_seam::clear_native_persistence();
    // Register the canonical BeatPulse vis - matches main.cpp's startup
    // wiring. Tests that exercise the "no vis registered" fallback path
    // can clear the registry locally inside the test body.
    visualisation_registry().register_plugin(beat_pulse_instance());

    dal::DAL::begin();
    ModeMachine::begin();
    // Skip past the Boot countdown.
    set_millis(6000);
    ModeMachine::loop_tick();
    ModeMachine::switch_to(ModeId::AutonomousMaster);
}

void tearDown(void) {}

// =============================================================================
// Picker overlay tests
// =============================================================================

static void test_picker_opens_and_closes_on_picker_action(void) {
    auto* m = master_instance();
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::None,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::Picker,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

static void test_picker_cycle_advances_cursor_and_wraps(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Picker);
    TEST_ASSERT_EQUAL_size_t(0u, m->overlay_cursor_for_tests());

    // Registry has 1 vis ("beat-pulse"); picker_row_count = 2
    // (vis + "<- Menu" sentinel).
    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(1u, m->overlay_cursor_for_tests());

    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(0u, m->overlay_cursor_for_tests());   // wrap
}

static void test_picker_confirm_vis_row_persists_selection(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Picker);
    // Cursor=0 -> "beat-pulse". Confirm should keep us in AutonomousMaster
    // and persist "beat-pulse" to NVS.
    inject_input_action(hal::InputAction::Confirm);

    TEST_ASSERT_EQUAL_INT((int)ModeId::AutonomousMaster,
                          (int)ModeMachine::current());
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::None,
        (int)m->overlay_for_tests());
    TEST_ASSERT_EQUAL_STRING("beat-pulse",
        modes::persistence::load_active_vis_id());
}

static void test_picker_back_menu_row_switches_to_menu(void) {
    inject_input_action(hal::InputAction::Picker);
    // Cycle once to land on "<- Menu" (row index 1 with one vis).
    inject_input_action(hal::InputAction::Cycle);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_INT((int)ModeId::Menu, (int)ModeMachine::current());
}

// =============================================================================
// Status-strip label
// =============================================================================

static void test_status_label_reflects_active_vis_display_name(void) {
    auto* m = master_instance();
    const char* label = m->status_label_for_tests();
    TEST_ASSERT_NOT_NULL(label);
    // BeatPulse's display_name is "Beat Pulse" (10 chars, within the
    // 10-char visible cap so no ellipsis). Whatever the vis returns,
    // the label must not be empty when a vis is active.
    TEST_ASSERT_NOT_EQUAL('\0', label[0]);
    TEST_ASSERT_EQUAL_STRING(beat_pulse_instance()->display_name(), label);
}

// =============================================================================
// Settings overlay
// =============================================================================

static void test_settings_opens_and_closes_on_settings_action(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Settings);
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::Settings,
        (int)m->overlay_for_tests());

    inject_input_action(hal::InputAction::Settings);
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

static void test_settings_confirm_on_enum_advances_value(void) {
    // BeatPulse's "color" property is the only schema entry: Enum [0..5],
    // default 1 (Red). Confirm on the first property row should advance
    // to 2 (Green).
    auto& bag = visualisations::beat_pulse_property_bag();
    bag.set("color", PropertyValue::from_enum(1));

    inject_input_action(hal::InputAction::Settings);
    inject_input_action(hal::InputAction::Confirm);   // edit row 0

    TEST_ASSERT_EQUAL_UINT8(2, bag.get("color").as_enum());
}

static void test_settings_confirm_on_enum_wraps_at_max(void) {
    auto& bag = visualisations::beat_pulse_property_bag();
    bag.set("color", PropertyValue::from_enum(5));    // max
    inject_input_action(hal::InputAction::Settings);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_UINT8(0, bag.get("color").as_enum()); // wrap to min
}

static void test_settings_back_row_closes_overlay(void) {
    auto* m = master_instance();
    inject_input_action(hal::InputAction::Settings);
    // BeatPulse has 1 property; back row sits at cursor=1.
    inject_input_action(hal::InputAction::Cycle);
    TEST_ASSERT_EQUAL_size_t(1u, m->overlay_cursor_for_tests());
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_INT(
        (int)AutonomousMasterMode::OverlayKind::None,
        (int)m->overlay_for_tests());
}

// =============================================================================
// NVS fallback - saved id that doesn't resolve falls back to beat-pulse.
// =============================================================================

static void test_unknown_saved_id_falls_back_to_beat_pulse(void) {
    // Save a garbage id, then re-enter AutonomousMaster: the resolver
    // should fall back to the registered "beat-pulse" instance.
    modes::persistence::save_active_vis_id("nonsense-id");
    ModeMachine::switch_to(ModeId::Menu);
    ModeMachine::switch_to(ModeId::AutonomousMaster);

    auto* m = master_instance();
    TEST_ASSERT_EQUAL_STRING("beat-pulse", m->active_vis_id_for_tests());
}

// =============================================================================
// Persisted picker selection survives a mode round-trip.
// =============================================================================

static void test_persisted_picker_selection_survives_round_trip(void) {
    // Open picker, confirm "beat-pulse" (cursor=0). Then leave and re-enter
    // AutonomousMaster; the persisted id should still resolve.
    inject_input_action(hal::InputAction::Picker);
    inject_input_action(hal::InputAction::Confirm);
    TEST_ASSERT_EQUAL_STRING("beat-pulse",
        modes::persistence::load_active_vis_id());

    ModeMachine::switch_to(ModeId::Menu);
    ModeMachine::switch_to(ModeId::AutonomousMaster);
    auto* m = master_instance();
    TEST_ASSERT_EQUAL_STRING("beat-pulse", m->active_vis_id_for_tests());
}

// =============================================================================
// Main
// =============================================================================

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_picker_opens_and_closes_on_picker_action);
    RUN_TEST(test_picker_cycle_advances_cursor_and_wraps);
    RUN_TEST(test_picker_confirm_vis_row_persists_selection);
    RUN_TEST(test_picker_back_menu_row_switches_to_menu);
    RUN_TEST(test_status_label_reflects_active_vis_display_name);
    RUN_TEST(test_settings_opens_and_closes_on_settings_action);
    RUN_TEST(test_settings_confirm_on_enum_advances_value);
    RUN_TEST(test_settings_confirm_on_enum_wraps_at_max);
    RUN_TEST(test_settings_back_row_closes_overlay);
    RUN_TEST(test_unknown_saved_id_falls_back_to_beat_pulse);
    RUN_TEST(test_persisted_picker_selection_survives_round_trip);
    return UNITY_END();
}
