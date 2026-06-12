// MenuMode implementation.

#include "menu_mode.h"

#include "persistence.h"
#include "dal/dal.h"

#include <cstdio>

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

namespace {

struct MenuItem {
    ModeId      target;
    const char* label;
};

constexpr MenuItem kMenuItems[] = {
    { ModeId::Director,  "Director Mode" },
    { ModeId::Lume,      "Lume Mode"     },
    { ModeId::Test,      "Test"          },
    { ModeId::Config,    "Config"        },
};
// DmxBridge was a top-level menu entry in the original Epic 7 B3 cut.
// Revised 2026-06-05 (Q4): it's now a Director-mode picker entry
// instead. See src/modes/director_mode.cpp::on_picker_confirm.
constexpr size_t kMenuItemCount = sizeof(kMenuItems) / sizeof(kMenuItems[0]);

}  // namespace

void MenuMode::enter() {
    // Default the cursor to whichever item is the persisted last-used
    // runtime mode, so a quick Btn1 press confirms the previous choice.
    selected_ = 0;
    const ModeId last = persistence::current_last_runtime();
    for (size_t i = 0; i < kMenuItemCount; ++i) {
        if (kMenuItems[i].target == last) {
            selected_ = i;
            break;
        }
    }
    draw();
}

void MenuMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed) return;
    if (ev.id == ButtonId::Btn2) {
        selected_ = (selected_ + 1) % kMenuItemCount;
        draw();
    } else if (ev.id == ButtonId::Btn1) {
        ModeMachine::switch_to(kMenuItems[selected_].target);
    }
}

void MenuMode::draw() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Mode", WHITE, BLACK, 3});
    for (size_t i = 0; i < kMenuItemCount; ++i) {
        const bool sel = (i == selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", kMenuItems[i].label);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 45 + (int)i * 18, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: select", WHITE, BLACK, 1});
}

}  // namespace modes
}  // namespace nocturnation
