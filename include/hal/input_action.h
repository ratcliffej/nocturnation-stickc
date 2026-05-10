// NocturNation HAL - semantic input actions.
//
// Visualisations (Block 5) and overlay UIs (Block 10) consume InputAction
// events instead of raw ButtonPressEvents so the same vis code runs
// unchanged across hosts with different button layouts (2-button StickC,
// 6-button Tildagon, ...).
//
// The host's input mapper translates raw (ButtonId, ButtonEvent) pairs
// into semantic InputAction events. Per-host mapping lives in
// src/hal/input_action_mapper_*.{h,cpp} (or src/hal_<host>/ for hosts
// that need a custom mapper). The 2-button mapper in
// src/hal/input_action_mapper_2btn is shared by the StickC Plus2 and S3
// backends. A future Tildagon HAL would add its own 6-button mapper.

#pragma once

#include <cstdint>

namespace nocturnation {
namespace hal {

// Semantic action emitted by the host's input mapper. Consumers
// (visualisations, overlay UI) subscribe to these instead of raw
// ButtonPressEvents so the same code runs unchanged on hosts with
// different button layouts.
enum class InputAction : uint8_t {
    None      = 0,
    Confirm,    // primary action: vis fire, overlay select, menu select
    Cycle,      // step forward through palette / band / option
    CyclePrev,  // step backward (may be unbound on hosts with too few buttons)
    Pause,      // pause / resume the active visualisation
    Settings,   // open / close the settings overlay for the active vis
    Picker,     // open / close the vis picker overlay; in menus = back-up
    AuxA,       // host-defined auxiliary action; unbound on 2-button hosts
    AuxB,
};

struct InputEvent {
    InputAction action       = InputAction::None;
    uint32_t    timestamp_ms = 0;
};

}  // namespace hal
}  // namespace nocturnation
