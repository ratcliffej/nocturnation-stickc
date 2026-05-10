// Native test: InputActionMapper2Btn (Epic 4.6 Block 4).
//
// Covers the 2-button mapping table:
//   - Btn1 Clicked       -> Confirm
//   - Btn2 Clicked       -> Cycle
//   - Btn1 LongPressed   -> Settings
//   - Btn2 LongPressed   -> Picker
//   - Btn1 DoubleClicked -> Pause
//
// Plus the negative-space invariants:
//   - Btn1 Pressed / Released emit nothing (Clicked is the canonical "tap").
//   - Btn2 DoubleClicked emits nothing (not in the 2-button mapping).
//   - Multiple events in sequence preserve order.
//   - Mapper with null Emit doesn't crash.
//
// Pure logic test; no HAL or DAL infrastructure required.

#include <unity.h>

#include <vector>

#include "hal/input_action_mapper_2btn.h"

using nocturnation::hal::ButtonEvent;
using nocturnation::hal::ButtonId;
using nocturnation::hal::InputAction;
using nocturnation::hal::InputActionMapper2Btn;
using nocturnation::hal::InputEvent;

namespace {

// Tiny record-and-replay sink so each test can assert on the InputEvent
// list a sequence of button events emitted.
struct Recorder {
    std::vector<InputEvent> events;
    InputActionMapper2Btn::Emit callback() {
        return [this](const InputEvent& ev) { events.push_back(ev); };
    }
};

}  // namespace

void setUp(void)    {}
void tearDown(void) {}

// -----------------------------------------------------------------------------
// Positive-space mapping coverage
// -----------------------------------------------------------------------------

void test_btn1_clicked_emits_confirm(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Clicked, 100);
    TEST_ASSERT_EQUAL_size_t(1, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Confirm),
                          static_cast<int>(rec.events[0].action));
    TEST_ASSERT_EQUAL_UINT32(100, rec.events[0].timestamp_ms);
}

void test_btn2_clicked_emits_cycle(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn2, ButtonEvent::Clicked, 250);
    TEST_ASSERT_EQUAL_size_t(1, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Cycle),
                          static_cast<int>(rec.events[0].action));
    TEST_ASSERT_EQUAL_UINT32(250, rec.events[0].timestamp_ms);
}

void test_btn1_longpressed_emits_settings(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::LongPressed, 500);
    TEST_ASSERT_EQUAL_size_t(1, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Settings),
                          static_cast<int>(rec.events[0].action));
}

void test_btn2_longpressed_emits_picker(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn2, ButtonEvent::LongPressed, 750);
    TEST_ASSERT_EQUAL_size_t(1, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Picker),
                          static_cast<int>(rec.events[0].action));
}

void test_btn1_doubleclicked_emits_pause(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::DoubleClicked, 1000);
    TEST_ASSERT_EQUAL_size_t(1, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Pause),
                          static_cast<int>(rec.events[0].action));
}

// -----------------------------------------------------------------------------
// Negative-space invariants
// -----------------------------------------------------------------------------

void test_btn1_pressed_emits_nothing(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Pressed, 10);
    TEST_ASSERT_EQUAL_size_t(0, rec.events.size());
}

void test_btn1_released_emits_nothing(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Released, 20);
    TEST_ASSERT_EQUAL_size_t(0, rec.events.size());
}

void test_btn2_doubleclicked_emits_nothing(void) {
    // Btn2 DoubleClicked is intentionally unbound in the 2-button mapping.
    // Hosts have to decide what to do with the action surface; we don't
    // emit something the host can't reach reliably.
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn2, ButtonEvent::DoubleClicked, 30);
    TEST_ASSERT_EQUAL_size_t(0, rec.events.size());
}

void test_btn3_emits_nothing(void) {
    // The Plus2 has a third hardware button (BtnPWR) wired as Btn3 in the
    // HAL, but the 2-button mapping deliberately ignores it - power
    // semantics are out of scope for the action surface.
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn3, ButtonEvent::Clicked,     40);
    m.on_button_event(ButtonId::Btn3, ButtonEvent::LongPressed, 50);
    TEST_ASSERT_EQUAL_size_t(0, rec.events.size());
}

// -----------------------------------------------------------------------------
// Sequence + degenerate-input invariants
// -----------------------------------------------------------------------------

void test_event_sequence_preserves_order(void) {
    Recorder rec;
    InputActionMapper2Btn m(rec.callback());
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Clicked,       100);
    m.on_button_event(ButtonId::Btn2, ButtonEvent::Clicked,       200);
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Pressed,       210);  // ignored
    m.on_button_event(ButtonId::Btn1, ButtonEvent::LongPressed,   300);
    m.on_button_event(ButtonId::Btn2, ButtonEvent::DoubleClicked, 320);  // ignored
    m.on_button_event(ButtonId::Btn2, ButtonEvent::LongPressed,   400);
    m.on_button_event(ButtonId::Btn1, ButtonEvent::DoubleClicked, 500);

    TEST_ASSERT_EQUAL_size_t(5, rec.events.size());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Confirm),
                          static_cast<int>(rec.events[0].action));
    TEST_ASSERT_EQUAL_UINT32(100, rec.events[0].timestamp_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Cycle),
                          static_cast<int>(rec.events[1].action));
    TEST_ASSERT_EQUAL_UINT32(200, rec.events[1].timestamp_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Settings),
                          static_cast<int>(rec.events[2].action));
    TEST_ASSERT_EQUAL_UINT32(300, rec.events[2].timestamp_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Picker),
                          static_cast<int>(rec.events[3].action));
    TEST_ASSERT_EQUAL_UINT32(400, rec.events[3].timestamp_ms);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(InputAction::Pause),
                          static_cast<int>(rec.events[4].action));
    TEST_ASSERT_EQUAL_UINT32(500, rec.events[4].timestamp_ms);
}

void test_null_emit_does_not_crash(void) {
    InputActionMapper2Btn m(nullptr);
    // Each branch of the mapping table - assertion is "doesn't crash".
    m.on_button_event(ButtonId::Btn1, ButtonEvent::Clicked,       1);
    m.on_button_event(ButtonId::Btn2, ButtonEvent::Clicked,       2);
    m.on_button_event(ButtonId::Btn1, ButtonEvent::LongPressed,   3);
    m.on_button_event(ButtonId::Btn2, ButtonEvent::LongPressed,   4);
    m.on_button_event(ButtonId::Btn1, ButtonEvent::DoubleClicked, 5);
    // Plus an unmapped event for completeness.
    m.on_button_event(ButtonId::Btn2, ButtonEvent::DoubleClicked, 6);
    TEST_PASS();
}

// -----------------------------------------------------------------------------

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_btn1_clicked_emits_confirm);
    RUN_TEST(test_btn2_clicked_emits_cycle);
    RUN_TEST(test_btn1_longpressed_emits_settings);
    RUN_TEST(test_btn2_longpressed_emits_picker);
    RUN_TEST(test_btn1_doubleclicked_emits_pause);
    RUN_TEST(test_btn1_pressed_emits_nothing);
    RUN_TEST(test_btn1_released_emits_nothing);
    RUN_TEST(test_btn2_doubleclicked_emits_nothing);
    RUN_TEST(test_btn3_emits_nothing);
    RUN_TEST(test_event_sequence_preserves_order);
    RUN_TEST(test_null_emit_does_not_crash);
    return UNITY_END();
}
