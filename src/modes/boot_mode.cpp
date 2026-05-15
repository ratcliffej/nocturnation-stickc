// BootMode implementation.

#include "boot_mode.h"

#include "firmware_version.h"
#include "persistence.h"
#include "dal/dal.h"

#include <cstdio>
#include <cstring>

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

// Splash countdown trimmed from 5 s to 3 s in Block 13 - 5 s felt
// overlong when an operator just wants to land in the persisted
// runtime mode. Three full-second tick changes still give clear
// "press button now" affordance without dragging the boot phase.
constexpr uint32_t kBootCountdownMs = 3000;

// Build identifier shown bottom-right of the splash. Sourced from the
// single canonical FIRMWARE_VERSION macro in include/firmware_version.h
// (Block 14 unified the splash and the System config screen on one
// value); the build system may still override via -DFIRMWARE_VERSION on
// one-off builds.
constexpr const char* kSplashVersion = ::nocturnation::kFirmwareVersion;

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
        case ModeId::Director: return "Master";
        case ModeId::Lume:            return "Slave";
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

    // Target-mode label sits just above the centred countdown digit.
    char target[24];
    std::snprintf(target, sizeof(target), "Entering %s",
                  mode_label(persistence::current_last_runtime()));
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, 60, target, WHITE, BLACK, 1});

    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kTitleX, 115, "press any btn for menu", WHITE, BLACK, 1});

    // Bottom-right corner status block: firmware build identifier plus
    // current battery percent. Size 1 white. Painted once in draw_static
    // (only-on-enter) since the splash window is short enough that
    // battery percent drift isn't worth a periodic refresh. Right-edge
    // anchored: kSplashVersion is short (e.g. "v0.5" = 4 chars = 24 px
    // at size 1) so we align by guessing length-times-6 from the right
    // margin.
    const int batt = DAL::battery_level("local");
    char status[32];
    if (batt < 0) {
        std::snprintf(status, sizeof(status), "%s  --%%", kSplashVersion);
    } else {
        std::snprintf(status, sizeof(status), "%s  %d%%",
                      kSplashVersion, batt);
    }
    const int status_chars = (int)std::strlen(status);
    const int status_x = 240 - status_chars * 6 - 4;   // 6 px/char at size 1
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        status_x, 125, status, WHITE, BLACK, 1});
}

void BootMode::draw_countdown(uint8_t seconds) {
    // Centred single yellow digit at size 3. Size-3 char cells are 18 px
    // wide; one digit -> x = (240 - 18) / 2 = 111. Painted with a
    // BLACK background so the previous frame's digit is overwritten in
    // place without flicker.
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%u", (unsigned)seconds);
    constexpr int kCountdownX = (240 - kCharW3) / 2;
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kCountdownX, 80, buf, YELLOW, BLACK, 3});
}

void BootMode::draw_pulsing_n(uint8_t step) {
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        kPulseNX, kTitleY, "N", pulse_color(step), BLACK, 3});
}

}  // namespace modes
}  // namespace nocturnation
