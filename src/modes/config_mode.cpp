// ConfigMode implementation.

#include "config_mode.h"

#include "firmware_version.h"
#include "persistence.h"
#include "dal/dal.h"
#include "../dal/drivers/local_driver.h"   // for set_pulse_enabled gating
#include "output_bindings/pixmob_ir.h"
#include "plugins/property_bag.h"

#include <cstdio>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#else
extern "C" uint32_t millis();
#endif

namespace nocturnation {
namespace modes {

using namespace nocturnation::dal;
using nocturnation::hal::ButtonId;
using nocturnation::hal::ButtonEvent;

// ConfigMode reads/writes operator-mutable settings. Channel + repeat
// for the slave route through persistence:: shared helpers (Block 9);
// the slave's IR forward group rides PixMobIrBinding's property bag.
namespace {

// Slave IR group cycles 0..5 via the binding's "group" property. Read
// hits the bag's NVS-backed (or, on native, in-memory) value; write
// clamps via PropertyValue::from_enum and the bag's bounds check.
uint8_t load_slave_ir_group() {
    return nocturnation::output_bindings::pixmob_ir_property_bag()
        .get("group").as_enum();
}

void save_slave_ir_group(uint8_t g) {
    if (g > 5) g = 0;
    nocturnation::output_bindings::pixmob_ir_property_bag().set(
        "group", plugins::PropertyValue::from_enum(g));
}

}  // namespace

// Out-of-class definitions for the ODR-used static constexpr members
// (preserved exactly from mode_machine.cpp).
constexpr ConfigMode::TopEntry ConfigMode::kTop[8];
constexpr const char* ConfigMode::kAudioItems[];
constexpr const char* ConfigMode::kIrItems[];
constexpr const char* ConfigMode::kEspNowItems[];
constexpr const char* ConfigMode::kWifiItems[];
constexpr const char* ConfigMode::kDmxItems[];

void ConfigMode::enter() {
    level_         = Level::Top;
    active_sub_    = SubMenu::None;
    top_selected_  = 0;
    sub_selected_  = 0;
    confirm_until_ms_ = 0;
    last_drawn_battery_ = -2;     // force first battery redraw
    draw();
}

void ConfigMode::loop_tick() {
    // System submenu has live read-outs (battery percent); refresh
    // every ~500 ms when shown. Confirmation flash for factory reset
    // also clears here.
    const uint32_t now = millis();
    if (confirm_until_ms_ != 0 && now >= confirm_until_ms_) {
        confirm_until_ms_ = 0;
        draw();
        return;
    }
    if (level_ == Level::Sub && active_sub_ == SubMenu::System) {
        const int batt = DAL::battery_level("local");
        if (batt != last_drawn_battery_) {
            last_drawn_battery_ = batt;
            draw();
        }
    }
}

void ConfigMode::on_button_event(const ButtonPressEvent& ev) {
    if (ev.kind != ButtonEvent::Pressed
     && ev.kind != ButtonEvent::LongPressed) return;

    // BtnB long-press pops one level. PixMob's two-level structure
    // (menu -> SetGroupId/GroupTarget workflow) gets an extra pop step
    // before it returns to the Config top-level.
    if (ev.id == ButtonId::Btn2 && ev.kind == ButtonEvent::LongPressed) {
        if (level_ == Level::Top) {
            ModeMachine::switch_to(ModeId::Menu);
        } else if (active_sub_ == SubMenu::PixMob
                && pixmob_state_ != PixMobState::Menu) {
            pixmob_state_     = PixMobState::Menu;
            confirm_until_ms_ = 0;
            draw();
        } else {
            level_      = Level::Top;
            active_sub_ = SubMenu::None;
            draw();
        }
        return;
    }

    if (ev.kind != ButtonEvent::Pressed) return;
    if (level_ == Level::Top) handle_top(ev);
    else                       handle_sub(ev);
}

void ConfigMode::handle_top(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        top_selected_ = (top_selected_ + 1) % kTopCount;
        draw();
    } else if (ev.id == ButtonId::Btn1) {
        level_         = Level::Sub;
        active_sub_    = kTop[top_selected_].target;
        sub_selected_  = 0;
        last_drawn_battery_ = -2;
        // Fresh entry into PixMob always lands on the menu screen
        // (not in a previous workflow).
        pixmob_state_     = PixMobState::Menu;
        pixmob_selected_  = 0;
        confirm_until_ms_ = 0;
        draw();
    }
}

void ConfigMode::draw_top() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Config", WHITE, BLACK, 2});
    for (size_t i = 0; i < kTopCount; ++i) {
        const bool sel = (i == top_selected_);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", kTop[i].label);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 22 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::handle_sub(const ButtonPressEvent& ev) {
    switch (active_sub_) {
        case SubMenu::System:  handle_system(ev);  break;
        case SubMenu::IR:      handle_ir(ev);      break;
        case SubMenu::Display: handle_display(ev); break;
        case SubMenu::EspNow:  handle_espnow(ev);  break;
        case SubMenu::PixMob:  handle_pixmob(ev);  break;
        case SubMenu::Audio:
        case SubMenu::WiFi:
        case SubMenu::Dmx:
            // Stub submenus accept Btn2 cycling for read-only browsing
            // of their planned-items list, but Btn1 is a no-op until
            // the relevant Epic wires real behaviour in.
            if (ev.id == ButtonId::Btn2) {
                sub_selected_ = (sub_selected_ + 1) % stub_item_count();
                draw();
            }
            break;
        default: break;
    }
}

void ConfigMode::draw_sub() {
    switch (active_sub_) {
        case SubMenu::Audio:   draw_stub("Audio", kAudioItems, kAudioItemCount, "Epic 3"); break;
        case SubMenu::Display: draw_display(); break;
        case SubMenu::IR:      draw_ir(); break;
        case SubMenu::EspNow:  draw_espnow(); break;
        case SubMenu::WiFi:   draw_stub("WiFi",    kWifiItems,   kWifiItemCount,   "Epic 4"); break;
        case SubMenu::Dmx:    draw_stub("DMX",     kDmxItems,    kDmxItemCount,    "Epic 7"); break;
        case SubMenu::PixMob: draw_pixmob(); break;
        case SubMenu::System: draw_system(); break;
        default: break;
    }
}

size_t ConfigMode::stub_item_count() const {
    switch (active_sub_) {
        case SubMenu::Audio:   return kAudioItemCount;
        case SubMenu::Display: return kDisplayFunctionalItemCount;
        case SubMenu::IR:      return kIrItemCount;
        case SubMenu::EspNow:  return kEspNowFunctionalItemCount;
        case SubMenu::WiFi:    return kWifiItemCount;
        case SubMenu::Dmx:     return kDmxItemCount;
        default:               return 1;
    }
}

void ConfigMode::draw_stub(const char* title,
                           const char* const* items, size_t count,
                           const char* epic_tag) {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, title, WHITE, BLACK, 2});
    char banner[24];
    std::snprintf(banner, sizeof(banner), "TBD %s", epic_tag);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        150, 10, banner, YELLOW, BLACK, 1});

    for (size_t i = 0; i < count; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", items[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// Display submenu (functional: Pulse Enable toggle + persists)
//
// Pulse Enable gates the LocalDriver's RgbPulse handler only - all
// other Display* output (status text, menus, etc.) keeps working
// when this is OFF. Useful when an operator wants the screen to
// show diagnostics/UI but not flash on beats.
// -------------------------------------------------------------------------

void ConfigMode::handle_display(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kDisplayFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1
     && (DisplayItem)sub_selected_ == DisplayItem::PulseEnable) {
        const bool next = !dal::local_driver_instance()->pulse_enabled();
        dal::local_driver_instance()->set_pulse_enabled(next);
        persistence::save_screen_pulse_enabled(next);
        draw();
    }
}

void ConfigMode::draw_display() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Display", WHITE, BLACK, 2});

    char ena[24];
    std::snprintf(ena, sizeof(ena), "Pulse: %s",
                  dal::local_driver_instance()->pulse_enabled()
                      ? "ON" : "OFF");
    const char* lines[kDisplayFunctionalItemCount] = { ena };

    for (size_t i = 0; i < kDisplayFunctionalItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// IR submenu (functional: Enable toggles + persists; Protocol/GroupID info)
// -------------------------------------------------------------------------

void ConfigMode::handle_ir(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kIrFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        if ((IRItem)sub_selected_ == IRItem::EnableDisable) {
            const bool next = !DAL::driver_enabled("ir-pixmob");
            DAL::set_driver_enabled("ir-pixmob", next);
            persistence::save_ir_enabled(next);
            draw();
        } else if ((IRItem)sub_selected_ == IRItem::SlaveGroup) {
            // Cycle 0 (broadcast / all-pixmobs) -> 1 .. 5 -> 0.
            uint8_t g = load_slave_ir_group();
            g = (g + 1) % 6;
            save_slave_ir_group(g);
            draw();
        }
    }
    // Protocol is info-only - no Btn1 action.
}

void ConfigMode::draw_ir() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "IR", WHITE, BLACK, 2});

    char ena[24];
    std::snprintf(ena, sizeof(ena), "Enable: %s",
                  DAL::driver_enabled("ir-pixmob") ? "ON" : "OFF");
    char grp[24];
    const uint8_t cur_grp = load_slave_ir_group();
    if (cur_grp == 0) {
        std::snprintf(grp, sizeof(grp), "Slave Grp: all");
    } else {
        std::snprintf(grp, sizeof(grp), "Slave Grp: %u", (unsigned)cur_grp);
    }
    const char* lines[kIrFunctionalItemCount] = {
        ena,
        "Protocol: PixMob",
        grp,
    };

    for (size_t i = 0; i < kIrFunctionalItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// ESP-NOW submenu (functional: Master Channel + Slave Channel)
//
// Per architecture spec §4.5 the project's two-channel social contract
// is channel 1 = hobby/community/open, channel 11 = show/commercial.
// Master picks 1, 11, or 6 (advanced override). Slave picks Auto (dual-
// channel scan with show priority) or locks to a specific channel.
// Both persist to NVS and survive reboot.
// -------------------------------------------------------------------------

const char* ConfigMode::master_channel_label(uint8_t c) {
    switch (c) {
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "1 hobby";
    }
}

const char* ConfigMode::slave_channel_label(uint8_t c) {
    switch (c) {
        case 0:  return "auto scan";
        case 1:  return "1 hobby";
        case 6:  return "6 custom";
        case 11: return "11 show";
        default: return "auto scan";
    }
}

uint8_t ConfigMode::cycle_master_channel(uint8_t c) {
    // 1 -> 6 -> 11 -> 1
    switch (c) {
        case 1:  return 6;
        case 6:  return 11;
        case 11: return 1;
        default: return 1;
    }
}

uint8_t ConfigMode::cycle_slave_channel(uint8_t c) {
    // 0 (auto) -> 1 -> 6 -> 11 -> 0
    switch (c) {
        case 0:  return 1;
        case 1:  return 6;
        case 6:  return 11;
        case 11: return 0;
        default: return 0;
    }
}

void ConfigMode::handle_espnow(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kEspNowFunctionalItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch ((EspNowItem)sub_selected_) {
            case EspNowItem::MasterChannel:
                persistence::save_master_channel(
                    cycle_master_channel(persistence::load_master_channel()));
                break;
            case EspNowItem::SlaveChannel:
                persistence::save_slave_channel(
                    cycle_slave_channel(persistence::load_slave_channel()));
                break;
            case EspNowItem::SlaveRepeat:
                persistence::save_slave_repeat_enabled(
                    !persistence::load_slave_repeat_enabled());
                break;
            case EspNowItem::SlaveGroup:
                // Increment 0..255 wrapping. A long-press fast-cycle is
                // a future polish if 256 single presses to wrap proves
                // tiresome; manual increment matches the brief.
                persistence::save_slv_group(
                    static_cast<uint8_t>(persistence::load_slv_group() + 1));
                break;
        }
        // New value applies on next AutonomousMaster / SlaveMode enter().
        // Operator returns to Menu and re-enters the mode to pick it up.
        draw();
    }
}

void ConfigMode::draw_espnow() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "ESP-NOW", WHITE, BLACK, 2});

    char m_line[28];
    char s_line[28];
    char r_line[28];
    char g_line[28];
    std::snprintf(m_line, sizeof(m_line), "Master: %s",
                  master_channel_label(persistence::load_master_channel()));
    std::snprintf(s_line, sizeof(s_line), "Slave:  %s",
                  slave_channel_label(persistence::load_slave_channel()));
    std::snprintf(r_line, sizeof(r_line), "Repeat: %s",
                  persistence::load_slave_repeat_enabled() ? "ON" : "OFF");
    std::snprintf(g_line, sizeof(g_line), "Group:  %u",
                  (unsigned)persistence::load_slv_group());
    const char* lines[kEspNowFunctionalItemCount] = { m_line, s_line, r_line, g_line };

    for (size_t i = 0; i < kEspNowFunctionalItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 80, "(applies on mode entry)",
        WHITE, BLACK, 1});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// PixMob submenu (PixMob-protocol-specific commands moved out of Test):
//   Set Group ID    bracelet-setup helper, fires SetGroupId+SetGroupSel
//   Group Target    Btn1 advances + fires to groups 1..5 in turn
//
// Two-level navigation within the submenu: menu lists the items, Btn1
// drills into a workflow screen. B-hold pops one level (workflow ->
// menu -> Config top).
// -------------------------------------------------------------------------

void ConfigMode::handle_pixmob(const ButtonPressEvent& ev) {
    switch (pixmob_state_) {
        case PixMobState::Menu:
            if (ev.id == ButtonId::Btn2) {
                pixmob_selected_ = (pixmob_selected_ + 1) % kPixMobItemCount;
                draw();
            } else if (ev.id == ButtonId::Btn1) {
                pixmob_state_ = (pixmob_selected_ == 0)
                                    ? PixMobState::SetGroupId
                                    : PixMobState::GroupTarget;
                confirm_until_ms_ = 0;
                draw();
            }
            break;
        case PixMobState::SetGroupId:
            if (ev.id == ButtonId::Btn2) {
                pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                draw();
            } else if (ev.id == ButtonId::Btn1) {
                // Target name is irrelevant - the AssignDeviceGroup
                // dispatch ignores device group_id and uses the event's
                // new_group_id payload.
                const bool ok = DAL::fire_assign_device_group(
                    "all-pixmobs",
                    AssignDeviceGroupEvent{pixmob_target_group_});
                if (ok) confirm_until_ms_ = millis() + kConfirmFlashMs;
                draw();
            }
            break;
        case PixMobState::GroupTarget:
            if (ev.id == ButtonId::Btn1) {
                char target[16];
                std::snprintf(target, sizeof(target),
                              "group-%u", (unsigned)pixmob_target_group_);
                DAL::fire_rgb_pulse(target, RgbPulseEvent{
                    0xFF, 0xFF, 0xFF,
                    pixmob::T_32_MS, pixmob::T_96_MS, pixmob::T_96_MS,
                    pixmob::CHANCE_100});
                pixmob_target_group_ = (pixmob_target_group_ % 5) + 1;
                draw();
            }
            break;
    }
}

void ConfigMode::draw_pixmob() {
    switch (pixmob_state_) {
        case PixMobState::Menu:        draw_pixmob_menu();      break;
        case PixMobState::SetGroupId:  draw_pixmob_set_group(); break;
        case PixMobState::GroupTarget: draw_pixmob_group_tgt(); break;
    }
}

void ConfigMode::draw_pixmob_menu() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "PixMob", WHITE, BLACK, 2});

    const char* items[kPixMobItemCount] = { "Set Group ID", "Group Test" };
    for (size_t i = 0; i < kPixMobItemCount; ++i) {
        const bool sel = (i == pixmob_selected_);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", items[i]);
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf,
            sel ? YELLOW : WHITE, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::draw_pixmob_set_group() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Set Group ID", WHITE, BLACK, 2});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "New group: %u",
                  (unsigned)pixmob_target_group_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 40, buf, WHITE, BLACK, 2});
    if (confirm_until_ms_ != 0) {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 70, "Sent!", GREEN, BLACK, 2});
    } else {
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 70, "isolate target!", YELLOW, BLACK, 1});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "B: cycle  A: send  B-hold: back",
        WHITE, BLACK, 1});
}

void ConfigMode::draw_pixmob_group_tgt() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "Group Test", WHITE, BLACK, 2});
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Next: group %u",
                  (unsigned)pixmob_target_group_);
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 50, buf, WHITE, BLACK, 2});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 128, "A: fire + advance  B-hold: back",
        WHITE, BLACK, 1});
}

// -------------------------------------------------------------------------
// System submenu (functional)
// -------------------------------------------------------------------------

void ConfigMode::handle_system(const ButtonPressEvent& ev) {
    if (ev.id == ButtonId::Btn2) {
        sub_selected_ = (sub_selected_ + 1) % kSystemItemCount;
        draw();
        return;
    }
    if (ev.id == ButtonId::Btn1) {
        switch ((SystemItem)sub_selected_) {
            case SystemItem::FactoryReset:
                factory_reset();
                confirm_until_ms_ = millis() + 800;   // "done" linger
                draw();
                break;
            default:
                // Info-only items - no-op on Btn1.
                break;
        }
    }
}

void ConfigMode::factory_reset() {
#ifdef ARDUINO
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.clear();
    prefs.end();
#endif
    // s_last_runtime stays as in-memory state until reboot.
}

void ConfigMode::draw_system() {
    DAL::fire_display_clear("local", DisplayClearEvent{BLACK});
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 5, "System", WHITE, BLACK, 2});

    char fw[28]; std::snprintf(fw, sizeof(fw), "Firmware: %s",
                               ::nocturnation::kFirmwareVersion);
    char dm[28]; std::snprintf(dm, sizeof(dm), "Default: %s",
                               mode_label_short(persistence::current_last_runtime()));
    char br[28];
    const int batt = last_drawn_battery_ >= -1 ? last_drawn_battery_
                                                : DAL::battery_level("local");
    if (batt < 0) std::snprintf(br, sizeof(br), "Batt: --");
    else          std::snprintf(br, sizeof(br), "Batt: %d%%", batt);

    const char* lines[kSystemItemCount] = {
        fw,
        dm,
        confirm_until_ms_ != 0 ? "Reset complete" : "Factory reset",
        br,
    };
    for (size_t i = 0; i < kSystemItemCount; ++i) {
        const bool sel = (i == sub_selected_);
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%s %s",
                      sel ? ">" : " ", lines[i]);
        const uint16_t fg = (i == (size_t)SystemItem::FactoryReset
                              && confirm_until_ms_ != 0) ? GREEN
                          : sel                          ? YELLOW
                          :                                WHITE;
        DAL::fire_display_show_text("local", DisplayShowTextEvent{
            10, 30 + (int)i * 16, buf, fg, BLACK, 2});
    }
    DAL::fire_display_show_text("local", DisplayShowTextEvent{
        10, 122, "B: cycle  A: select  B-hold: back",
        WHITE, BLACK, 1});
}

const char* ConfigMode::mode_label_short(ModeId m) {
    switch (m) {
        case ModeId::AutonomousMaster: return "Master";
        case ModeId::Slave:            return "Slave";
        case ModeId::Config:           return "Config";
        case ModeId::Test:             return "Test";
        default:                       return "?";
    }
}

void ConfigMode::draw() {
    if (level_ == Level::Top) draw_top();
    else                       draw_sub();
}

}  // namespace modes
}  // namespace nocturnation
