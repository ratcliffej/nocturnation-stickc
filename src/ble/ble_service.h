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
#define BLE_PAIRING_WINDOW_S_DEFAULT 180
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

// Central-role scan state machine (Epic 20 B10). BLE central and
// peripheral coexist on the same NimBLE stack; the two are used
// mutually-exclusively from UX (Config > BLE Pair uses peripheral,
// Config > Config Lumes uses central).
enum class ScanState : uint8_t {
    Idle     = 0,
    Scanning = 1,
    Done     = 2,
};

// One discovered Lume peripheral (Epic 20 B10).
struct DiscoveredLume {
    uint8_t bt_mac[6];        // BLE peripheral address (big-endian, for display) for connect()
    uint8_t addr_type;        // BLE_ADDR_PUBLIC / _RANDOM / etc. Captured from ADV so
                              // configure_lume() rebuilds the correct NimBLEAddress.
    char    adv_name[24];     // Advertising name as seen on-air; NUL-terminated
    int8_t  rssi;             // Signal strength at last advertisement
    bool    valid;            // Slot occupied
};

// Result of a configure_lume() call. Distinguished so the UI can show
// specific error text; on any non-Ok, no bytes were persisted on the
// remote device (the write either failed or was refused).
enum class ConfigureResult : uint8_t {
    Ok                 = 0,
    ConnectFailed      = 1,
    ServiceNotFound    = 2,
    CharacteristicNotFound = 3,
    WriteFailed        = 4,
    CommitFailed       = 5,
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
    // True while a BLE central is connected to us. Set by NimBLE server
    // callbacks; consumers use it for LED/screen "someone's talking"
    // indication and the pairing-window pause (see tick()).
    bool         client_connected() const { return client_connected_; }

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
    // Called from NimBLE server callbacks on client connect / disconnect.
    // Pauses the pairing-window timeout while a client is talking to us
    // (so a slow phone / laptop CLI walking the config doesn't fall off
    // the window mid-write) and drives the "connected" LED / screen
    // indication.
    void on_client_connected();
    void on_client_disconnected();

    // ---- BLE central role (Epic 20 B10) ----
    // Scan for NocturNation Lume peripherals for up to duration_ms.
    // Discovered devices land in the fixed-size internal list; the
    // caller polls scan_state() / discovered() from the main loop.
    // Returns false if the stack failed to enter central mode. Safe
    // to call while the peripheral service is inactive (they can
    // coexist on ESP32 but our UX keeps them mutually exclusive).
    static constexpr size_t kMaxDiscovered = 8;
    bool      start_scan(uint32_t duration_ms);
    void      stop_scan();
    ScanState scan_state() const { return scan_state_; }
    const DiscoveredLume* discovered() const { return discovered_; }
    size_t                discovered_count() const { return discovered_count_; }

    // Callback used by the NimBLE scan runner to feed a discovered
    // device into the list. Public for the same friend-avoidance
    // reason as the pairing callbacks.
    void on_scan_result(const uint8_t bt_mac[6],
                        uint8_t addr_type,
                        const char* adv_name,
                        int8_t rssi);
    void on_scan_complete();

    // Connect to a discovered Lume as BLE central, write the property-
    // bag TLV to the config characteristic, follow with a
    // pairing_control::commit, disconnect. Blocking; takes 1-3 s.
    // UI shows a spinner during the call. Returns a specific error
    // code so the caller can render meaningful feedback.
    ConfigureResult configure_lume(const DiscoveredLume& target,
                                   const uint8_t* bag_tlv,
                                   size_t bag_len);

    // Connect to a discovered Lume as BLE central, read the current
    // property bag from the config characteristic, disconnect (Epic 20
    // B11). Blocking; takes ~500 ms. On Ok, `out_bag` holds the raw
    // TLV bytes and `in_out_len` is updated to the actual byte count.
    // On any error `in_out_len` is set to 0. Uses the same connect
    // hardening as configure_lume (re-scan, connect-by-device, explicit
    // connection params).
    ConfigureResult read_lume_config(const DiscoveredLume& target,
                                     uint8_t* out_bag,
                                     size_t& in_out_len);

private:
    // role_label was removed 2026-09-20 alongside the "NTN" short-name
    // rewrite — the advertising name no longer carries the role (see
    // compose_adv_name in the .cpp). Role is still surfaced via
    // device_info.role for any client that needs to discriminate.

    bool    active_        = false;
    Role    role_          = Role::Lume;
    Host    host_          = Host::StickCPlus2;
    uint8_t pair_win_s_    = kPairingWindowSecondsDefault;
    uint8_t bt_mac_[6]     = {};  // captured at begin() from esp_bt_dev_get_address
    char    adv_name_[24]  = {};  // NCTN-Director-XXXXXX + NUL headroom
    PairingState pairing_state_ = PairingState::Closed;
    uint32_t     window_started_ms_ = 0;
    bool         write_seen_        = false;
    // BLE-client-attached state (bench 2026-09-20: pairing window was
    // firing timeout mid-config because the operator was still
    // interacting with the phone / another Director). Track connect
    // state so tick() can pause the timeout while a client is present.
    bool         client_connected_  = false;
    // Timestamp of the last disconnect so tick() can shift
    // window_started_ms_ forward by the "connected" duration and give
    // the operator the full window back once the client drops.
    uint32_t     connected_at_ms_   = 0;

    // Central-role scan state (Epic 20 B10).
    ScanState      scan_state_       = ScanState::Idle;
    DiscoveredLume discovered_[kMaxDiscovered] = {};
    size_t         discovered_count_ = 0;
};

// Process-wide accessor. Firmware picks this up from menu / pairing UX
// blocks in B4; nothing calls it in v0x01 B3a beyond the smoke tests.
BleService& ble_service();

}  // namespace ble
}  // namespace nocturnation
