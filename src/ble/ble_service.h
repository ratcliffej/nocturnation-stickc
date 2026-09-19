// NocturNation BLE GATT service — Epic 20 (see Docs/manuals/ble-service.md).
//
// Host-agnostic: this TU compiles into every StickC / AtomLite / AtomS3-*
// firmware env and drives NimBLE on the ESP32. The service is only
// advertised while begin()..end() bracket a pairing window; the rest of
// the time the BLE radio is off (§8 coexistence rule — full ESP-NOW
// airtime during a show).
//
// v0x01 characteristics are wired to persistence in B3c: device_info
// reads return the 24-byte struct; config reads emit a role-appropriate
// property bag; config writes are gated on the pairing window and apply
// each recognised key to NVS; pairing_control drives commit / abort /
// commit-and-sleep transitions; show_passthrough writes are refused with
// status 0x81 (declared-but-not-implemented per spec §3.5). Status
// notify is deferred to a follow-on (v0x01 exposes read-only status).
//
// Not native-safe: the implementation .cpp assumes the Arduino / NimBLE
// runtime. Native tests exercise the property-bag TLV codec (B3b) which
// lives in its own host-agnostic TU.

#pragma once

#include <cstddef>
#include <cstdint>

namespace nocturnation {
namespace ble {

// Roles surfaced in device_info.role (see Docs/manuals/ble-service.md §3.1).
enum class Role : uint8_t {
    Director = 0x01,
    Lume     = 0x02,
};

// Hosts surfaced in device_info.host. Numbering matches the spec table.
enum class Host : uint8_t {
    StickCPlus2   = 0x01,
    StickCS3      = 0x02,
    AtomLite      = 0x03,
    AtomS3Lite    = 0x04,
    AtomS3PoE     = 0x05,
    Tildagon      = 0x06,   // MicroPython side; here for enum parity only.
};

// Service and characteristic UUIDs (128-bit, self-assigned). Kept in a
// single header so the spec doc, firmware, and future tests reference
// the same strings. Changing any UUID is a service-version bump.
namespace uuid {
constexpr const char* kService         = "2086dee9-a671-44b1-ae81-123829bd4c88";
constexpr const char* kDeviceInfo      = "37b33d23-b02d-4705-be47-b19c57a981ee";
constexpr const char* kConfig          = "381b3227-28f7-4036-9ff0-cb76a6e43430";
constexpr const char* kStatus          = "52a398a9-2c10-4291-885e-2a98a8f4d54e";
constexpr const char* kPairingControl  = "72678cdb-1b65-417c-8744-751eff153bf0";
constexpr const char* kShowPassthrough = "17cc926e-f45e-41d8-ab61-1cb71d744c35";
constexpr const char* kDiagnostics     = "2b53e72d-0e3d-4083-9f76-d5d472a26356";
}  // namespace uuid

// Service-version byte reported by device_info. Bump on any incompatible
// wire change to the characteristics.
constexpr uint8_t kServiceVersion = 0x01;

// Pairing-window duration. NVS-configurable per Docs/manuals/ble-service.md
// §5; this header holds the compile-time default that persistence.h reads
// as the first-boot value. Build-flag override:
//     -DBLE_PAIRING_WINDOW_S_DEFAULT=N
#ifndef BLE_PAIRING_WINDOW_S_DEFAULT
#define BLE_PAIRING_WINDOW_S_DEFAULT 30
#endif
constexpr uint8_t kPairingWindowSecondsDefault = BLE_PAIRING_WINDOW_S_DEFAULT;

// Not implemented in v0x01 — status code returned by show_passthrough writes
// so a client can detect the feature-gate. Also returned by config writes
// attempted outside the pairing window (with slightly different semantics —
// see Docs/manuals/ble-service.md §3.2). Consumers should check both the
// characteristic and the code before drawing conclusions.
constexpr uint8_t kStatusNotInPairingWindow   = 0x81;
constexpr uint8_t kStatusMalformedTlv         = 0x82;
constexpr uint8_t kStatusValueOutOfRange      = 0x83;

// Pairing-window state machine (transitions per spec §7).
enum class PairingState : uint8_t {
    Closed    = 0,   // Service not running; no advertising, no writes accepted.
    Open      = 1,   // Advertising; config writes accepted while in this state.
    Committed = 2,   // Client wrote pairing_control::commit — window closes on tick.
    Sleeping  = 3,   // commit_and_sleep — window closes and firmware enters deep sleep.
    Aborted   = 4,   // Client wrote pairing_control::abort or gesture cancelled.
};

// BleService is the process-wide singleton owning the NimBLE stack.
// begin(role, host, pair_win_s) starts advertising with the role's UUID +
// name; tick() drives the pairing-window timeout; end() tears the stack
// down. is_active() reports state.
//
// Threading: NimBLE characteristic callbacks fire on the BLE host task
// (single-threaded per NimBLE contract). BleService member state that
// callbacks touch is `pairing_state_` (uint8_t writes are atomic on
// ESP32) and the persistence functions the callbacks call take their
// own NVS locks. begin() and end() are only called from the main task
// (Config-mode menu handlers, per B4). tick() is called from Config
// mode's loop_tick.
class BleService {
public:
    // Idempotent: begin() while active is a no-op that returns true.
    // Returns false only if NimBLE initialisation fails at OS level.
    // `pair_win_s == 0` uses the compile-time default.
    bool begin(Role role, Host host, uint8_t pair_win_s);

    // Poll from the main task's loop tick. Closes the window on timeout;
    // returns the current state so callers can update UI (countdown,
    // success/failure flash, sleep transition).
    PairingState tick();

    // Idempotent: end() while inactive is a no-op.
    void end();

    bool is_active() const { return active_; }

    // Accessors for UI blocks (B4).
    PairingState pairing_state() const { return pairing_state_; }
    uint8_t      pair_win_s()    const { return pair_win_s_; }
    uint32_t     seconds_remaining() const;
    bool         should_sleep()  const { return pairing_state_ == PairingState::Sleeping; }
    bool         write_seen()    const { return write_seen_; }

    // Advertised name for the current begin() call. Format:
    //     NCTN-<Director|Lume>-<hex(bt_mac[3..5])>
    // Or the operator-set friendly_name if configured (empty falls back
    // to the MAC-derived form). Fills buf and returns the number of
    // chars written (excluding NUL). Returns 0 if the service is not
    // active OR buf is too small (need >= 21 chars).
    size_t advertising_name(char* buf, size_t buflen) const;

    Role role() const { return role_; }
    Host host() const { return host_; }
    const uint8_t* bt_mac() const { return bt_mac_; }

    // Called from NimBLE characteristic callbacks (BLE host task).
    // Public so the callback dispatchers in ble_service.cpp can reach
    // them without friend declarations; not for firmware external use.
    void on_pairing_control_write(uint8_t action);
    void on_config_write_success();

private:
    static const char* role_label(Role r);

    bool    active_        = false;
    Role    role_          = Role::Lume;
    Host    host_          = Host::StickCPlus2;
    uint8_t pair_win_s_    = kPairingWindowSecondsDefault;
    uint8_t bt_mac_[6]     = {};  // captured at begin() from esp_bt_dev_get_address
    char    adv_name_[24]  = {};  // NCTN-Director-XXXXXX + NUL headroom
    PairingState pairing_state_ = PairingState::Closed;
    uint32_t     window_started_ms_ = 0;
    bool         write_seen_        = false;
};

// Process-wide accessor. Firmware picks this up from menu / pairing UX
// blocks in B4; nothing calls it in v0x01 B3a beyond the smoke tests.
BleService& ble_service();

}  // namespace ble
}  // namespace nocturnation
