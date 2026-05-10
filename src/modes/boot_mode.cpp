// BootMode implementation.

#include "boot_mode.h"

#include "persistence.h"
#include "dal/dal.h"

#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

namespace {

constexpr uint32_t kBootCountdownMs = 5000;

}

void BootMode::enter() {
    start_ms_           = millis();
    last_drawn_seconds_ = 0xFF;
    last_drawn_pulse_   = 0xFF;
    draw_static();
}

void BootMode::loop_tick() {
    const uint32_t now     = millis();
    const uint32_t elapsed = now - start_ms_;
    if (elapsed >= kBootCountdownMs) {
        ModeMachine::switch_to(persistence::current_last_runtime());
        return;
    }
    const uint8_t remaining = (uint8_t)((kBootCountdownMs - elapsed) / 1000) + 1;
    if (remaining != last_drawn_seconds_) {
        last_drawn_seconds_ = remaining;
        draw_countdown(remaining);
    }
    // Pulse the brand-mark N. 16 phases per cycle * 120 ms each gives
    // a ~2 s breathe. Anchor to start_ms_ so phase 0 (full brightness)
    // is always the first frame after entering Boot - the splash opens
    // bright instead of mid-cycle. Redraw only the single character
    // cell so there's no flicker on the rest of the splash.
    const uint8_t step = (uint8_t)(((now - start_ms_) / 120) & 0x0F);
    if (step != last_drawn_pulse_) {
        last_drawn_pulse_ = step;
        draw_pulsing_n(step);
    }
}

void BootMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.kind == ButtonEvent::Pressed || ev.kind == ButtonEvent::Clicked) {
        ModeMachine::switch_to(ModeId::Menu);
    }
}

// Cosine-shaped brightness ramp over 16 phases, scaled to [178, 255]
// - peaks at 255 (full bright) on phase 0 and dips ~30% to 178 on
// phase 8. The N stays clearly visible the whole cycle; it never
// dims to black. Orange/yellow tone (full red, ~half green, no blue).
uint16_t BootMode::pulse_color(uint8_t step) {
    static constexpr uint8_t kBrightness[16] = {
        255, 252, 244, 231, 217, 202, 189, 181,
        178, 181, 189, 202, 217, 231, 244, 252,
    };
    const uint8_t b  = kBrightness[step & 0x0F];
    const uint8_t r5 = b >> 3;                          // 0..31
    const uint8_t g6 = ((uint16_t)b * 50 / 100) >> 2;   // ~half intensity
    return ((uint16_t)r5 << 11) | ((uint16_t)g6 << 5);
}

const char* BootMode::mode_label(ModeId m) {
    switch (m) {
        case ModeId::AutonomousMaster: return "Master";
        case ModeId::Slave:            return "Slave";
        case ModeId::Config:           return "Config";
        case ModeId::Test:             return "Test";
        default:                       return "?";
    }
}

void BootMode::draw_static() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    // Brand title. "Noctur" + (pulsing N drawn separately) + "ation".
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, kTitleY, "Noctur", WHITE, BLACK, 3});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX + 7 * kCharW3, kTitleY, "ation", WHITE, BLACK, 3});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, kTitleY + 28, "Open-source crowd lighting.",
        WHITE, BLACK, 1});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, 115, "press any btn for menu", WHITE, BLACK, 1});
}

void BootMode::draw_countdown(uint8_t seconds) {
    // Fixed-width format ("X in N s" - one digit) so subsequent draws
    // overwrite the previous text cell-for-cell with no flicker.
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%s in %u s",
                  mode_label(persistence::current_last_runtime()),
                  (unsigned)seconds);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, 75, buf, WHITE, BLACK, 2});
}

void BootMode::draw_pulsing_n(uint8_t step) {
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kPulseNX, kTitleY, "N", pulse_color(step), BLACK, 3});
}

}  // namespace modes
}  // namespace nocturnation
