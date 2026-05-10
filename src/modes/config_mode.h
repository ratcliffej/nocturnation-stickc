// ConfigMode - the §8.4 config tree.
//
// Two-level navigation: top-level submenu list -> submenu items. Btn2
// cycles selection at either level. Btn1 enters a submenu from the top
// level, and activates an item within a submenu (toggling, cycling, or
// triggering depending on item type). PWR-hold pops one level (submenu
// -> top, top -> mode menu).
//
// Pre-Epic-4 / pre-Epic-7 status: the menu *shape* is built out per spec
// §8.4 so users can see what config will exist. Functional leaves at
// this milestone live under System (firmware version, default boot mode
// info, factory reset, battery status). Audio / IR / ESP-NOW / WiFi /
// DMX submenus list their planned items as labels but are non-
// interactive; they fill in when the relevant transport / capability
// epics ship.

#pragma once

#include <cstddef>
#include <cstdint>

#include "modes/mode_machine.h"

namespace nocturnation {
namespace modes {

class ConfigMode : public Mode {
public:
    ModeId id() const override { return ModeId::Config; }
    const char* name() const override { return "Config"; }

    void enter() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;

private:
    enum class Level   : uint8_t { Top, Sub };
    enum class SubMenu : uint8_t {
        None = 0, Audio, Display, IR, EspNow, WiFi, Dmx, PixMob, System,
    };

    // Within the PixMob submenu, drilling into one of its items enters a
    // workflow sub-state with its own button bindings. Menu = the list
    // (Set Group ID / Group Target); SetGroupId / GroupTarget = the two
    // workflow screens.
    enum class PixMobState : uint8_t { Menu, SetGroupId, GroupTarget };

    struct TopEntry {
        SubMenu     target;
        const char* label;
    };
    static constexpr TopEntry kTop[8] = {
        { SubMenu::Audio,   "Audio"   },
        { SubMenu::Display, "Display" },
        { SubMenu::IR,      "IR"      },
        { SubMenu::EspNow,  "ESP-NOW" },
        { SubMenu::WiFi,    "WiFi"    },
        { SubMenu::Dmx,     "DMX"     },
        { SubMenu::PixMob,  "PixMob"  },
        { SubMenu::System,  "System"  },
    };
    static constexpr size_t kTopCount = sizeof(kTop) / sizeof(kTop[0]);

    Level       level_              = Level::Top;
    SubMenu     active_sub_         = SubMenu::None;
    size_t      top_selected_       = 0;
    size_t      sub_selected_       = 0;
    uint32_t    confirm_until_ms_   = 0;
    int         last_drawn_battery_ = -2;

    PixMobState pixmob_state_       = PixMobState::Menu;
    size_t      pixmob_selected_    = 0;
    uint8_t     pixmob_target_group_ = 1;
    static constexpr size_t kPixMobItemCount = 2;
    static constexpr uint32_t kConfirmFlashMs = 800;     // "Sent!" linger

    // Top-level navigation.
    void handle_top(const dal::ButtonPressEvent& ev);
    void draw_top();

    // Submenu dispatch.
    void handle_sub(const dal::ButtonPressEvent& ev);
    void draw_sub();

    // Stub-submenu data (planned items per spec §8.4; non-interactive).
    static constexpr const char* kAudioItems[] = {
        "Enable / Disable", "Show FFT spectrum", "Show beat meter", "Tuning",
    };
    static constexpr size_t kAudioItemCount = sizeof(kAudioItems) / sizeof(kAudioItems[0]);

    static constexpr const char* kIrItems[] = {
        "Enable / Disable", "Protocol", "Group ID assignment",
    };
    static constexpr size_t kIrItemCount = sizeof(kIrItems) / sizeof(kIrItems[0]);

    static constexpr const char* kEspNowItems[] = {
        "Enable / Disable", "Channel number", "Source ID",
    };
    static constexpr size_t kEspNowItemCount = sizeof(kEspNowItems) / sizeof(kEspNowItems[0]);

    static constexpr const char* kWifiItems[] = {
        "Enable / Disable", "SSID", "Password", "Soft-AP mode",
    };
    static constexpr size_t kWifiItemCount = sizeof(kWifiItems) / sizeof(kWifiItems[0]);

    static constexpr const char* kDmxItems[] = {
        "Carrier", "Universe ID", "Channel mapping",
    };
    static constexpr size_t kDmxItemCount = sizeof(kDmxItems) / sizeof(kDmxItems[0]);

    size_t stub_item_count() const;

    void draw_stub(const char* title,
                   const char* const* items, size_t count,
                   const char* epic_tag);

    // Display submenu (functional: Pulse Enable toggle + persists).
    enum class DisplayItem : uint8_t {
        PulseEnable = 0,
    };
    static constexpr size_t kDisplayFunctionalItemCount = 1;

    void handle_display(const dal::ButtonPressEvent& ev);
    void draw_display();

    // IR submenu (functional: Enable toggles + persists; Protocol/GroupID info).
    enum class IRItem : uint8_t {
        EnableDisable = 0,
        Protocol,
        SlaveGroup,
    };
    static constexpr size_t kIrFunctionalItemCount = 3;

    void handle_ir(const dal::ButtonPressEvent& ev);
    void draw_ir();

    // ESP-NOW submenu (functional: Master Channel + Slave Channel).
    enum class EspNowItem : uint8_t {
        MasterChannel = 0,
        SlaveChannel,
        SlaveRepeat,
    };
    static constexpr size_t kEspNowFunctionalItemCount = 3;

    static const char* master_channel_label(uint8_t c);
    static const char* slave_channel_label(uint8_t c);
    static uint8_t cycle_master_channel(uint8_t c);
    static uint8_t cycle_slave_channel(uint8_t c);

    void handle_espnow(const dal::ButtonPressEvent& ev);
    void draw_espnow();

    // PixMob submenu.
    void handle_pixmob(const dal::ButtonPressEvent& ev);
    void draw_pixmob();
    void draw_pixmob_menu();
    void draw_pixmob_set_group();
    void draw_pixmob_group_tgt();

    // System submenu (functional).
    enum class SystemItem : uint8_t {
        FirmwareVersion = 0,
        DefaultBootMode,
        FactoryReset,
        BatteryStatus,
    };
    static constexpr size_t kSystemItemCount = 4;

    void handle_system(const dal::ButtonPressEvent& ev);
    void factory_reset();
    void draw_system();

    static const char* mode_label_short(ModeId m);

    static constexpr const char* kFirmwareVersion = "1.0.0";

    void draw();
};

}  // namespace modes
}  // namespace nocturnation
