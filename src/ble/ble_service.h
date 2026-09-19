// NocturNation BLE GATT service — Epic 20 (see Docs/manuals/ble-service.md).
//
// Host-agnostic: this TU compiles into every StickC / AtomLite / AtomS3-*
// firmware env and drives NimBLE on the ESP32. The service is only
// advertised while begin()..end() bracket a pairing window; the rest of
// the time the BLE radio is off (§8 coexistence rule — full ESP-NOW
// airtime during a show).
//
// v0x01 is the skeleton: service registration, empty characteristics,
// role/host/BT-MAC-based advertising. Characteristic behaviour (property-
// bag decode, permission gating, notify) lands in B3c.
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

// BleService is the process-wide singleton owning the NimBLE stack.
// begin(role, host, pair_win_s) starts advertising with the role's UUID +
// name; end() tears the stack down. is_active() reports state.
//
// Threading: NimBLE callbacks fire on the BLE task; this class must
// tolerate begin()/end() from the main loop and characteristic access
// from the BLE task. In v0x01 there is no characteristic data yet, so
// the concurrency surface is minimal.
class BleService {
public:
    // Idempotent: begin() while active is a no-op that returns true.
    // Returns false only if NimBLE initialisation fails at OS level.
    bool begin(Role role, Host host, uint8_t pair_win_s);

    // Idempotent: end() while inactive is a no-op.
    void end();

    bool is_active() const { return active_; }

    // Advertised name for the current begin() call. Format:
    //     NCTN-<Director|Lume>-<hex(bt_mac[3..5])>
    // Fills buf and returns the number of chars written (excluding NUL).
    // Returns 0 if the service is not active OR buf is too small (need >=
    // 21 chars).
    size_t advertising_name(char* buf, size_t buflen) const;

private:
    static const char* role_label(Role r);

    bool    active_        = false;
    Role    role_          = Role::Lume;
    Host    host_          = Host::StickCPlus2;
    uint8_t pair_win_s_    = kPairingWindowSecondsDefault;
    uint8_t bt_mac_[6]     = {};  // captured at begin() from esp_bt_dev_get_address
    char    adv_name_[24]  = {};  // NCTN-Director-XXXXXX + NUL headroom
};

// Process-wide accessor. Firmware picks this up from menu / pairing UX
// blocks in B4; nothing calls it in v0x01 B3a beyond the smoke tests.
BleService& ble_service();

}  // namespace ble
}  // namespace nocturnation
