// MenuMode - Btn2 cycles, Btn1 selects.

#pragma once

#include <cstddef>

#include "modes/mode_machine.h"

namespace nocturnation {
namespace modes {

class MenuMode : public Mode {
public:
    ModeId id() const override { return ModeId::Menu; }
    const char* name() const override { return "Menu"; }

    void enter() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    size_t selected_ = 0;

    void draw();
};

}  // namespace modes
}  // namespace nocturnation
