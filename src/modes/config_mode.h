// ConfigMode - the operator-facing settings tree.
//
// Three-level navigation: top-level list -> optional picker -> leaf
// submenu. Btn2 cycles selection at every level. Btn1 either descends
// (Top -> Picker -> Sub) or activates a leaf item (toggle / cycle /
// trigger depending on item type); a leaf item can also be a direct
// action right at Top (Group ID). B-hold pops one level:
//
//   Sub (reached via Picker)  -> Picker
//   Sub (reached from Top)    -> Top
//   Picker                    -> Top
//   Top                       -> mode menu (exits Config)
//
// Top-level shape (5 items):
//   Group: N       direct action - A-click increments wrapping 0..6
//   Display        existing submenu (drill straight in)
//   Connectivity   picker -> IR / ESP-NOW / WiFi / DMX
//   Utilities      picker -> PixMob
//   System         existing submenu (drill straight in)
//
// FSM shape choice: explicit Level::Picker plus active_picker_ +
// picker_selected_ alongside the existing active_sub_ + sub_selected_.
// A path stack would over-engineer two transient levels; a "came from"
// sentinel hides the back-target inside Sub state and is harder to
// reason about. The explicit level keeps the state machine readable.

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
    void exit() override;
    void loop_tick() override;
    void on_button_event(const dal::ButtonPressEvent& ev) override;
    void on_audio_frame (const dal::AudioFrameEvent&    ev) override;

private:
    enum class Level   : uint8_t { Top, Picker, Sub };

    // Top-level entries. SubMenu::None marks a direct-action top item
    // (no drill-down). SubMenu::Connectivity / Utilities mark picker
    // top items (drill into a level-2 picker). Everything else drills
    // straight into a leaf submenu.
    enum class SubMenu : uint8_t {
        None = 0,
        // Pickers (level-2 lists).
        Connectivity,
        Utilities,
        // Leaf submenus.
        Show,
        Display,
        IR,
        EspNow,
        WiFi,
        Dmx,
        PixMob,
        LevelTuning,
        System,
        // Epic 12 B4 stub. Functional wiring (NVS-backed enable +
        // brightness cap + group/repeat data model) lands once the
        // Plus2 / S3 HAL backends gain a real LedStrip on the Grove
        // port; until then this leaf renders read-only via the
        // shared draw_stub() template.
        LedStrip,
    };

    // Within the PixMob submenu, drilling into one of its items enters a
    // workflow sub-state with its own button bindings. Menu = the list
    // (Set Group ID / Group Target); SetGroupId / GroupTarget = the two
    // workflow screens.
    enum class PixMobState : uint8_t { Menu, SetGroupId, GroupTarget };

    // Top-level item shape. A Top entry is either:
    //   (a) a direct action: target == SubMenu::None, handled inline at
    //       Top via the GroupAction discriminant; or
    //   (b) a picker: target is Connectivity or Utilities, drills into
    //       a level-2 picker; or
    //   (c) a leaf submenu: target is the leaf, drill straight in.
    enum class TopAction : uint8_t {
        Drill,      // descend into target (picker or leaf)
        GroupId,    // direct action: increment slv_group 0..6 (UI cap; wire allows 0..255)
    };
    struct TopEntry {
        SubMenu     target;
        TopAction   action;
        const char* label;
    };
    static constexpr TopEntry kTop[6] = {
        { SubMenu::None,         TopAction::GroupId, "Group"        },
        { SubMenu::Show,         TopAction::Drill,   "Show"         },
        { SubMenu::Display,      TopAction::Drill,   "Display"      },
        { SubMenu::Connectivity, TopAction::Drill,   "Connectivity" },
        { SubMenu::Utilities,    TopAction::Drill,   "Utilities"    },
        { SubMenu::System,       TopAction::Drill,   "System"       },
    };
    static constexpr size_t kTopCount = sizeof(kTop) / sizeof(kTop[0]);

    // Connectivity picker entries (level-2 list under Connectivity).
    struct PickerEntry {
        SubMenu     target;
        const char* label;
    };
    static constexpr PickerEntry kConnectivity[5] = {
        { SubMenu::IR,       "IR"        },
        { SubMenu::EspNow,   "ESP-NOW"   },
        { SubMenu::WiFi,     "WiFi"      },
        { SubMenu::Dmx,      "DMX"       },
        { SubMenu::LedStrip, "LED Strip" },   // Epic 12 B4 (stub)
    };
    static constexpr size_t kConnectivityCount =
        sizeof(kConnectivity) / sizeof(kConnectivity[0]);

    // Utilities picker entries (level-2 list under Utilities).
    // Level Tuning (Epic 4.7 Block 2) hosts the widget library
    // standalone for bench work: manual injection drives the IR /
    // ESP-NOW path independently of audio so a developer can verify
    // the display + transport without playing music.
    // Wash Test (Epic 6C) moved to Test Mode (test_mode.h) so it sits
    // alongside the rest of the §8.5 hardware-verification catalogue;
    // Utilities is for developer bench tools, not operator-facing tests.
    static constexpr PickerEntry kUtilities[2] = {
        { SubMenu::PixMob,      "PixMob"       },
        { SubMenu::LevelTuning, "Level Tuning" },
    };
    static constexpr size_t kUtilitiesCount =
        sizeof(kUtilities) / sizeof(kUtilities[0]);

    Level       level_              = Level::Top;
    SubMenu     active_picker_      = SubMenu::None;
    SubMenu     active_sub_         = SubMenu::None;
    size_t      top_selected_       = 0;
    size_t      picker_selected_    = 0;
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

    // Picker level (Connectivity / Utilities).
    void handle_picker(const dal::ButtonPressEvent& ev);
    void draw_picker();
    const PickerEntry* picker_entries() const;
    size_t             picker_count() const;
    const char*        picker_title() const;

    // Submenu dispatch.
    void handle_sub(const dal::ButtonPressEvent& ev);
    void draw_sub();

    // Stub-submenu data (planned items per spec §8.4; non-interactive).
    static constexpr const char* kWifiItems[] = {
        "Enable / Disable", "SSID", "Password", "Soft-AP mode",
    };
    static constexpr size_t kWifiItemCount = sizeof(kWifiItems) / sizeof(kWifiItems[0]);

    static constexpr const char* kDmxItems[] = {
        "Carrier", "Universe ID", "Channel mapping",
    };
    static constexpr size_t kDmxItemCount = sizeof(kDmxItems) / sizeof(kDmxItems[0]);

    // LED Strip submenu items - all live. Btn1 toggles / cycles the
    // selected value through the table below in handle_led_strip:
    //   Enable      -> ON / OFF
    //   Brightness  -> 100 / 10 / 1 %
    //   Group size  -> 6 / 12 / 24 pixels-per-CHANCE-roll
    //   Chain size  -> 10 cm (15) / 20 cm (29) / 50 cm (72) / 1 m (144) / 2 m (288)
    enum class LedStripItem : uint8_t {
        Enable = 0,
        Brightness,
        GroupSize,
        ChainSize,
    };
    static constexpr size_t kLedStripItemCount = 4;

    size_t stub_item_count() const;

    void draw_stub(const char* title,
                   const char* const* items, size_t count,
                   const char* epic_tag);

    // Show submenu (functional: picker over registered shows). Cycles
    // through show_registry, persists active_show on confirm. Mirrors
    // the DirectorMode picker but reachable from Config without
    // entering Director mode (Epic 4.7 Block 1).
    void handle_show(const dal::ButtonPressEvent& ev);
    void draw_show();

    // Level Tuning submenu (Epic 4.7 Block 2). Live audio drives the
    // BeatBarWidget (flux ratio with threshold marker at 2.5x baseline)
    // and the SpectrumBarsWidget (7-band perceptual roll-up from the
    // AudioFrameEvent's per-band summary). Btn2 cycles the display
    // mode (Live / 25 / 50 / 75 / 100 %); the four percentage modes
    // override the widgets to fixed values so a developer can verify
    // the IR + ESP-NOW path without audio. Btn1 fires a render_fx
    // pulse at the currently-displayed level (live ratio or fixed %).
    enum class LevelTuningMode : uint8_t {
        Live = 0,
        Pct25,
        Pct50,
        Pct75,
        Pct100,
    };
    static constexpr size_t kLevelTuningModeCount = 5;
    LevelTuningMode level_tuning_mode_ = LevelTuningMode::Live;

    // Audio state captured by on_audio_frame while the sub is active.
    bool      level_tuning_audio_active_  = false;
    float     level_tuning_prev_bass_     = 0.0f;
    float     level_tuning_baseline_      = 100.0f;
    float     level_tuning_flux_          = 0.0f;
    float     level_tuning_level_rms_     = 0.0f;
    float     level_tuning_spectrum_[7]   = {0, 0, 0, 0, 0, 0, 0};
    uint32_t  last_level_tuning_draw_ms_  = 0;
    uint32_t  last_system_redraw_ms_      = 0;

    void handle_level_tuning(const dal::ButtonPressEvent& ev);
    void draw_level_tuning();
    // Audio lifecycle helpers - called when the operator transitions
    // into / out of the Level Tuning sub-mode. Starts / stops mic
    // capture so audio frames only flow while the widgets are visible.
    void level_tuning_audio_enter();
    void level_tuning_audio_exit();
    // Translate LevelTuningMode to its display label and override
    // fraction. Static so the helpers don't widen the public surface.
    static float       mode_to_fraction(LevelTuningMode m);
    static const char* mode_label(LevelTuningMode m);

    // Display submenu (functional: Pulse Enable toggle + persists).
    enum class DisplayItem : uint8_t {
        PulseEnable = 0,
    };
    static constexpr size_t kDisplayFunctionalItemCount = 1;

    void handle_display(const dal::ButtonPressEvent& ev);
    void draw_display();

    // IR submenu (functional: Enable toggle + Protocol info). PixMob
    // protocol IR group is now relay-driven via inbound target_group
    // (Epic 4.65 Block 5); the per-binding "group" property still
    // exists as broadcast fallback and is surfaced via the auto-
    // generated settings overlay, but no longer lives on this screen.
    enum class IRItem : uint8_t {
        EnableDisable = 0,
        InternalEmitter,   // present only when an external emitter exists
        ExternalEmitter,   // present only when an external emitter exists
        Protocol,
    };
    // The IR submenu length is board-dependent: backends with a second
    // (external) IR emitter expose the Internal/External emitter selection;
    // backends with only the built-in LED show just Enable + Protocol (the
    // original layout). ir_item_count()/ir_item_at() map a list position to
    // the item it represents so the rest of the menu code stays position-
    // driven. "Has external" is probed via hal::HAL::ir_tx_ext() != nullptr.
    static size_t ir_item_count();
    static IRItem ir_item_at(size_t pos);

    void handle_ir(const dal::ButtonPressEvent& ev);
    void draw_ir();

    // LED Strip submenu (Epic 12). Items kept stub-style except for
    // Brightness, which is live: Btn1 cycles 100 / 10 / 1 % through
    // the same level table the Btn1-in-Lume gesture uses, with the
    // value persisted to NVS via persistence::save_strip_brightness().
    // Enable, Group size, Repeat are reserved labels for now; future
    // B4-live wiring lights them up.
    void handle_led_strip(const dal::ButtonPressEvent& ev);
    void draw_led_strip();

    // ESP-NOW submenu (functional: Director Channel + Director ID +
    // Lume Channel + Lume Repeat). The Lume Group setting moved to
    // the top-level Config > Group item (direct action).
    //
    // DirectorId (added 2026-06-24, EMF prep) shows the device's
    // persisted Performance-range source id and A-click re-rolls +
    // saves a new random one. Operator-facing recourse when two
    // Directors at a multi-stage venue end up with the same id (low
    // probability, ~1 in 190, but operationally useful to surface).
    enum class EspNowItem : uint8_t {
        MasterChannel = 0,
        DirectorId,
        SlaveChannel,
        SlaveRepeat,
    };
    static constexpr size_t kEspNowFunctionalItemCount = 4;

    static const char* director_channel_label(uint8_t c);
    static const char* lume_channel_label(uint8_t c);
    static uint8_t cycle_director_channel(uint8_t c);
    static uint8_t cycle_lume_channel(uint8_t c);

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

    void draw();
};

}  // namespace modes
}  // namespace nocturnation
