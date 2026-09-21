// NocturNation BLE GATT service implementation (Epic 20 B3c).
//
// Wires the five mandatory characteristics + the reserved show_passthrough
// to their v0x01 behaviour:
//
//   device_info   — 24-byte struct read (role, host, fw_ver, wire_ver,
//                   bt_mac, uptime_s).
//   config        — read emits a role-appropriate property bag via the
//                   TLV codec (B3b); write is gated on the pairing
//                   window and applies each recognised key to NVS.
//   status        — 12-byte struct read (running_mode, lock_state,
//                   source_id, battery, signal, airtime_drops). Notify
//                   deferred; v0x01 is read-only.
//   pairing_ctrl  — 1-byte action write (commit / commit_and_sleep /
//                   abort).
//   show_pt       — write refused with status 0x81 per §3.5.
//
// Non-Arduino builds compile only the trivial state-machine methods so
// higher-level code can include ble_service.h.

#include "ble_service.h"

#include <cstdio>
#include <cstring>

#include "property_bag_tlv.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_bt_device.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include "../modes/persistence.h"
#endif

namespace nocturnation {
namespace ble {

namespace {
BleService s_instance;

// Firmware version reported over BLE. Kept as compile-time constants so
// the on-device value tracks the built binary rather than an NVS field.
// Update on each firmware release (matches the human-readable version
// documented on the device / in release notes).
constexpr uint8_t kFwMajor = 0;
constexpr uint8_t kFwMinor = 5;
constexpr uint8_t kFwPatch = 0;

// Wire protocol version this firmware speaks over ESP-NOW (§4 protocol
// manual). Bump when the wire evolves; independent of BLE service version.
constexpr uint8_t kWireVersion = 0x04;

}  // namespace

BleService& ble_service() { return s_instance; }

// -----------------------------------------------------------------------------
// Arduino / NimBLE path
// -----------------------------------------------------------------------------

#ifdef ARDUINO

namespace {

// The characteristic pointers are held in the anonymous namespace so the
// callback classes below can push notifications / read the current values.
// Only touched during begin() / end() / callbacks (all serialised).
NimBLECharacteristic* s_char_device_info = nullptr;
NimBLECharacteristic* s_char_config      = nullptr;
NimBLECharacteristic* s_char_status      = nullptr;
NimBLECharacteristic* s_char_pair_ctrl   = nullptr;
NimBLECharacteristic* s_char_show_pt     = nullptr;

// Compose the advertising name, honouring an operator-set friendly_name
// override when present. Returns chars written (excluding NUL).
//
// Bench 2026-09-20: dropped the role prefix ("Dir" / "Lume") from the
// name. Motivations:
//   (1) an Atom acting as a Director (driven by a phone app or USB
//       serial) is a planned future variant — the role isn't a stable
//       display identity, and clients that need it read
//       device_info.role over BLE anyway.
//   (2) name-length is a limited BLE budget; even with the UUID in the
//       scan response, a shorter name gives more headroom for
//       friendly_name and future extensions.
// Fallback form is `NTN%02X%02X%X` (8 chars): "NTN" prefix + 20 bits
// from bt_mac[3..5]. ~1M-value collision space is enough for the
// realistic small fleets we deploy.
size_t compose_adv_name(char* buf, size_t buflen, const uint8_t* mac) {
    if (buflen < 9 || !buf || !mac) return 0;
    // Try friendly_name first. Non-empty: use verbatim (already clamped
    // to 20 bytes by save_friendly_name).
    char friendly[24] = {};
    const size_t fn_len = modes::persistence::load_friendly_name(friendly, sizeof(friendly));
    if (fn_len > 0 && fn_len < buflen) {
        std::memcpy(buf, friendly, fn_len);
        buf[fn_len] = '\0';
        return fn_len;
    }
    const int n = std::snprintf(
        buf, buflen,
        "NTN%02X%02X%X",
        static_cast<unsigned>(mac[3]),
        static_cast<unsigned>(mac[4]),
        static_cast<unsigned>((mac[5] >> 4) & 0x0F));
    return (n < 0) ? 0 : static_cast<size_t>(n);
}

bool fetch_bt_mac(uint8_t out[6]) {
    // `esp_bt_dev_get_address()` is documented to return NULL until
    // the BT controller is fully enabled and the address has been
    // programmed into the host stack, which raced our call site on
    // first pairing gesture (bench 2026-09-19 — name showed as
    // "NCTN-Dir-000000"). `esp_read_mac(ESP_MAC_BT)` reads the value
    // straight from eFUSE via the ROM API instead, so it's valid as
    // soon as the chip is out of reset.
    if (esp_read_mac(out, ESP_MAC_BT) == ESP_OK) return true;
    // Fallback: try the BT-stack accessor if eFUSE read fails. Shouldn't
    // happen on ESP32; kept as belt-and-braces.
    const uint8_t* addr = esp_bt_dev_get_address();
    if (!addr) return false;
    std::memcpy(out, addr, 6);
    return true;
}

// ---- device_info -----------------------------------------------------------

class DeviceInfoCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) override {
        auto& svc = ble_service();
        uint8_t buf[24] = {};
        buf[0] = kServiceVersion;
        buf[1] = static_cast<uint8_t>(svc.role());
        buf[2] = static_cast<uint8_t>(svc.host());
        buf[3] = kFwMajor;
        buf[4] = kFwMinor;
        buf[5] = kFwPatch;
        buf[6] = kWireVersion;
        buf[7] = 0;
        std::memcpy(&buf[8], svc.bt_mac(), 6);
        const uint32_t up_s = ::millis() / 1000u;
        buf[14] = static_cast<uint8_t>( up_s        & 0xFF);
        buf[15] = static_cast<uint8_t>((up_s >>  8) & 0xFF);
        buf[16] = static_cast<uint8_t>((up_s >> 16) & 0xFF);
        buf[17] = static_cast<uint8_t>((up_s >> 24) & 0xFF);
        // 18..23 reserved (zero)
        chr->setValue(buf, sizeof(buf));
    }
};

// ---- config ---------------------------------------------------------------

// Serialise the current NVS state into a property bag matching the device's
// role. Called on config-characteristic read.
size_t serialise_config_bag(uint8_t* buf, size_t buflen, Role role) {
    TlvEncoder enc(buf, buflen);
    if (role == Role::Director) {
        // Director bag: sid_perf, retx_count, pair_win_s, friendly_name.
        enc.add_u8 (key::kDirSidPerf,
                    modes::persistence::load_director_perf_source_id());
        enc.add_u8 (key::kRetxCount,
                    modes::persistence::load_retx_count());
        enc.add_u8 (key::kPairWinS,
                    modes::persistence::load_pair_win_s());
        char name[24] = {};
        const size_t nlen = modes::persistence::load_friendly_name(name, sizeof(name));
        if (nlen > 0) {
            enc.add_utf8(key::kFriendlyName, name, static_cast<uint8_t>(nlen));
        }
    } else {
        // Lume bag: group, led_power, bound_sid (skipped in v0x01 — the
        // NVS key + consumer land in a follow-on epic), channel_pref,
        // strip topology, pair_win_s, friendly_name.
        enc.add_u8 (key::kGroup,
                    modes::persistence::load_lume_group());
        enc.add_u8 (key::kLedPower,
                    modes::persistence::load_strip_brightness());
        enc.add_u8 (key::kChannelPref,
                    modes::persistence::load_lume_channel());
        enc.add_u16(key::kStripChain,
                    modes::persistence::load_strip_chain_size());
        enc.add_u8 (key::kStripGroupSize,
                    modes::persistence::load_strip_group_size());
        enc.add_u8 (key::kPairWinS,
                    modes::persistence::load_pair_win_s());
        char name[24] = {};
        const size_t nlen = modes::persistence::load_friendly_name(name, sizeof(name));
        if (nlen > 0) {
            enc.add_utf8(key::kFriendlyName, name, static_cast<uint8_t>(nlen));
        }
    }
    return enc.size();
}

// Callback context: apply one entry from a decoded property bag. Returns
// true to continue iterating, false only if the whole bag should be
// rejected (which the codec surfaces as CallbackAbort). We accept
// unrecognised keys silently so forward-compat holds.
struct ApplyCtx {
    Role role;
    bool value_out_of_range;
};

bool apply_config_entry(const TlvEntry& e, void* raw_ctx) {
    auto* ctx = static_cast<ApplyCtx*>(raw_ctx);
    // Copy the key to a small NUL-terminated buffer for strcmp.
    char keybuf[32] = {};
    const size_t klen = e.key_len < sizeof(keybuf) - 1 ? e.key_len : sizeof(keybuf) - 1;
    std::memcpy(keybuf, e.key, klen);
    keybuf[klen] = '\0';

    auto read_u8 = [&](uint8_t& out) -> bool {
        if (e.type != ValueType::U8 || e.value_len != 1 || !e.value) return false;
        out = e.value[0];
        return true;
    };

    if (ctx->role == Role::Director) {
        if (std::strcmp(keybuf, key::kDirSidPerf) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            if (v < 0x40 || v > 0xFE) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_director_perf_source_id(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kRetxCount) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_retx_count(v);
            return true;
        }
    } else {
        if (std::strcmp(keybuf, key::kGroup) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_lume_group(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kLedPower) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_strip_brightness(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kChannelPref) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_lume_channel(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kStripChain) == 0) {
            if (e.type != ValueType::U16 || e.value_len != 2 || !e.value) {
                ctx->value_out_of_range = true; return true;
            }
            const uint16_t v = static_cast<uint16_t>(e.value[0])
                             | (static_cast<uint16_t>(e.value[1]) << 8);
            modes::persistence::save_strip_chain_size(v);
            return true;
        }
        if (std::strcmp(keybuf, key::kStripGroupSize) == 0) {
            uint8_t v;
            if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
            modes::persistence::save_strip_group_size(v);
            return true;
        }
    }

    // Keys common to both roles.
    if (std::strcmp(keybuf, key::kPairWinS) == 0) {
        uint8_t v;
        if (!read_u8(v)) { ctx->value_out_of_range = true; return true; }
        modes::persistence::save_pair_win_s(v);
        return true;
    }
    if (std::strcmp(keybuf, key::kFriendlyName) == 0) {
        if (e.type != ValueType::Utf8) { ctx->value_out_of_range = true; return true; }
        char clipped[21] = {};
        const size_t n = e.value_len < 20 ? e.value_len : 20;
        for (size_t i = 0; i < n; ++i) clipped[i] = static_cast<char>(e.value[i]);
        clipped[n] = '\0';
        modes::persistence::save_friendly_name(clipped);
        return true;
    }

    // Unknown key — silently accept (forward-compat, §4).
    return true;
}

class ConfigCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) override {
        uint8_t buf[240] = {};
        const size_t n = serialise_config_bag(buf, sizeof(buf), ble_service().role());
        chr->setValue(buf, n);
    }

    void onWrite(NimBLECharacteristic* chr) override {
        auto& svc = ble_service();
        if (svc.pairing_state() != PairingState::Open) {
            Serial.println("[ble] config write refused: pairing window closed");
            return;
        }
        const std::string value = chr->getValue();
        ApplyCtx ctx{ svc.role(), /*value_out_of_range=*/false };
        const DecodeError err = decode_property_bag(
            reinterpret_cast<const uint8_t*>(value.data()),
            value.size(),
            apply_config_entry,
            &ctx);
        if (err != DecodeError::Ok) {
            Serial.printf("[ble] config write malformed: err=%u\n", static_cast<unsigned>(err));
            return;
        }
        if (ctx.value_out_of_range) {
            Serial.println("[ble] config write: some entries out of range (partial apply)");
        }
        svc.on_config_write_success();
        Serial.printf("[ble] config write applied: %u bytes\n",
                      static_cast<unsigned>(value.size()));
    }
};

// ---- status ---------------------------------------------------------------

class StatusCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) override {
        // 12-byte status struct per spec §3.3. Values are best-effort in
        // v0x01 — Director source_id / airtime_drops / battery would flow
        // in from the driver + HAL once a subsequent block wires them.
        // For now: zeros with a role-correct running_mode.
        uint8_t buf[12] = {};
        const Role r = ble_service().role();
        buf[0] = (r == Role::Director) ? 0x03  // Config mode (pairing UX enters from Config)
                                        : 0x11; // Lume-running
        buf[1] = 0x00;
        buf[2] = 0x00;
        buf[3] = 0x00;
        buf[4] = 0xFF;   // battery unknown
        buf[5] = 0xFF;   // signal not applicable
        // airtime_drops[6..9]: zero
        // reserved[10..11]: zero
        chr->setValue(buf, sizeof(buf));
    }
};

// ---- pairing_control -------------------------------------------------------

class PairingControlCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onWrite(NimBLECharacteristic* chr) override {
        const std::string value = chr->getValue();
        if (value.empty()) return;
        const uint8_t action = static_cast<uint8_t>(value[0]);
        ble_service().on_pairing_control_write(action);
    }
};

// ---- show_passthrough (reserved) ------------------------------------------

class ShowPassthroughCallbacks : public NimBLECharacteristicCallbacks {
public:
    void onRead(NimBLECharacteristic* chr) override {
        // Sentinel bag: { reserved: u8=0x01 } — signals feature-gate to
        // clients without triggering an error.
        uint8_t buf[16] = {};
        TlvEncoder enc(buf, sizeof(buf));
        enc.add_u8("reserved", 0x01);
        chr->setValue(buf, enc.size());
    }

    void onWrite(NimBLECharacteristic*) override {
        // Refused; a client that read the sentinel bag knows this write
        // isn't wired. Log so bench traffic is visible.
        Serial.println("[ble] show_passthrough write refused: not implemented in v0x01");
    }
};

// Static instances so the callback pointers stay valid for the service
// lifetime. NimBLE holds the pointer — no ownership transfer.
DeviceInfoCallbacks       s_cb_device_info;
ConfigCallbacks           s_cb_config;
StatusCallbacks           s_cb_status;
PairingControlCallbacks   s_cb_pair_ctrl;
ShowPassthroughCallbacks  s_cb_show_pt;

// Server-connection tracking (bench 2026-09-20). Previously tried a
// NimBLEServerCallbacks subclass, but v1.4.2 has multiple onConnect
// signatures and only dispatched to one per event — our override was
// never called. Switched to polling s_server->getConnectedCount() from
// tick() below; a lot more robust and doesn't depend on which overload
// NimBLE chose today.

// Bench-observed 2026-09-19: `NimBLEDevice::deinit(clearAll=true)` on
// end() reliably crashes + reboots the Stick when the operator cancels
// pairing while a client is still connected (NimBLE frees the server
// while the host task is mid-disconnect). Fix: keep the NimBLE stack
// initialised for the lifetime of the process and only stop / restart
// advertising per pairing window. Characteristic pointers stay valid;
// begin() only touches them on the first call.
bool s_stack_initialized = false;
NimBLEServer* s_server = nullptr;

}  // namespace

bool BleService::begin(Role role, Host host, uint8_t pair_win_s) {
    if (active_) return true;

    role_       = role;
    host_       = host;
    pair_win_s_ = (pair_win_s == 0) ? kPairingWindowSecondsDefault : pair_win_s;
    if (pair_win_s_ < 5)   pair_win_s_ = 5;

    // ---- One-shot stack init -------------------------------------------
    // Runs at most once per process. NimBLE stays up across cancel /
    // re-pair cycles so end() can avoid the crashy deinit path.
    if (!s_stack_initialized) {
        NimBLEDevice::init("NocturNation");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);

        if (!fetch_bt_mac(bt_mac_)) {
            Serial.println("[ble] warn: BT MAC read failed; adv name will have zero suffix");
            std::memset(bt_mac_, 0, sizeof(bt_mac_));
        }

        s_server = NimBLEDevice::createServer();
        // NB: no setCallbacks() — connect/disconnect edges are tracked
        // via polling in tick() instead, because NimBLE-Arduino v1.4.2
        // dispatches to varying onConnect overloads and our override
        // wasn't reliably called. See tick() below.
        NimBLEService* svc = s_server->createService(uuid::kService);

        s_char_device_info = svc->createCharacteristic(uuid::kDeviceInfo,
                                                        NIMBLE_PROPERTY::READ);
        s_char_device_info->setCallbacks(&s_cb_device_info);

        s_char_config      = svc->createCharacteristic(uuid::kConfig,
                                                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
        s_char_config->setCallbacks(&s_cb_config);

        s_char_status      = svc->createCharacteristic(uuid::kStatus,
                                                        NIMBLE_PROPERTY::READ);
        s_char_status->setCallbacks(&s_cb_status);

        s_char_pair_ctrl   = svc->createCharacteristic(uuid::kPairingControl,
                                                        NIMBLE_PROPERTY::WRITE);
        s_char_pair_ctrl->setCallbacks(&s_cb_pair_ctrl);

        s_char_show_pt     = svc->createCharacteristic(uuid::kShowPassthrough,
                                                        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
        s_char_show_pt->setCallbacks(&s_cb_show_pt);

        svc->start();

        NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
        adv->setMinInterval(0x20);
        adv->setMaxInterval(0x40);
        // Bench 2026-09-20: keep the service UUID in the primary ADV
        // and skip the scan-response entirely. Previous attempt split
        // UUID to scan-response (motivated by NCTN-Lume-XXXXXX name
        // overflow), but that path left the StickC's central-role
        // connect() timing out at 8 s with NimBLE status=13 — the
        // Atom advertised but wasn't accepting connections. Now that
        // the name is 8 chars ("NTNXXXXX"), the whole thing fits:
        // Flags(3) + LocalName(2+8=10) + UUID(2+16=18) = 31 bytes,
        // exactly at the primary ADV cap.
        adv->setScanResponse(false);
        adv->addServiceUUID(NimBLEUUID(uuid::kService));
        // Explicit connectable-undirected mode so there's no ambiguity
        // about whether central-role peers can connect.
        adv->setAdvertisementType(BLE_GAP_CONN_MODE_UND);

        s_stack_initialized = true;
    } else {
        // Re-entry: BT MAC + service are already set up; only refresh
        // fields that may have changed since the last window (currently
        // just the advertising name via friendly_name).
        if (!fetch_bt_mac(bt_mac_)) {
            std::memset(bt_mac_, 0, sizeof(bt_mac_));
        }
    }

    // Advertising name is recomposed every begin() so friendly_name
    // writes during the previous pairing session take effect on the
    // next one.
    compose_adv_name(adv_name_, sizeof(adv_name_), bt_mac_);
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->setName(adv_name_);
    if (!adv->start()) {
        Serial.println("[ble] advertising start FAILED");
        return false;
    }

    active_             = true;
    pairing_state_      = PairingState::Open;
    window_started_ms_  = ::millis();
    write_seen_         = false;
    // Fresh session — reset connect-tracking state so a lingering flag
    // from a previous session doesn't fool tick() into pausing forever.
    client_connected_   = false;
    connected_at_ms_    = 0;
    Serial.printf("[ble] up: name=%s uuid=%s window=%us\n",
                  adv_name_, uuid::kService, (unsigned)pair_win_s_);
    return true;
}

PairingState BleService::tick() {
    if (!active_ || pairing_state_ == PairingState::Closed) {
        return pairing_state_;
    }
    // ---- Poll server connection state (bench 2026-09-20) ------------
    // Reliable replacement for the NimBLEServerCallbacks route: the
    // callback signatures vary across NimBLE-Arduino versions and our
    // override wasn't fired. Polling `getConnectedCount()` at loop
    // cadence (~20 Hz) catches connect/disconnect edges more than
    // fast enough for pairing UX.
    const bool now_connected = (s_server && s_server->getConnectedCount() > 0);
    if (now_connected && !client_connected_) {
        on_client_connected();
    } else if (!now_connected && client_connected_) {
        on_client_disconnected();
    }
    // ---- Auto-timeout, PAUSED while a client is connected ----------
    // The Open→timeout path auto-closes to Aborted so a walk-away
    // operator doesn't leave a stray advertising session running.
    // Paused while `client_connected_` because a phone/laptop walking
    // the config shouldn't get ejected mid-write.
    if (pairing_state_ == PairingState::Open && !client_connected_) {
        const uint32_t elapsed_ms = ::millis() - window_started_ms_;
        if (elapsed_ms >= static_cast<uint32_t>(pair_win_s_) * 1000u) {
            pairing_state_ = PairingState::Aborted;
            Serial.printf("[ble] tick -> Aborted (elapsed=%lu ms, pair_win_s=%u)\n",
                          (unsigned long)elapsed_ms, (unsigned)pair_win_s_);
        }
    }
    return pairing_state_;
}

void BleService::end() {
    if (!active_) return;
    // Stop advertising first so no new client can land while we
    // gracefully close existing connections.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    if (adv) {
        adv->stop();
    }
    // Gracefully disconnect any live client. NimBLE frees connection
    // resources on the host task; the callback fires before the next
    // pairing session so re-entry sees a clean slate. This is where
    // the pre-hotfix code crashed: calling deinit(clearAll=true) here
    // freed the server pointer while the disconnect callback was mid-
    // flight, dereferencing freed memory when the operator hit cancel.
    if (s_server && s_server->getConnectedCount() > 0) {
        // Server::disconnect() with just a conn_handle iterates through
        // all currently connected peers via the server's connection
        // list. NimBLE-Arduino v1.4.2's public API is best-effort; on
        // the two hosts benched to date, iterating conn_handle 0..N-1
        // covers the common case (one client at a time).
        for (uint16_t i = 0; i < s_server->getConnectedCount(); ++i) {
            (void)s_server->disconnect(i);
        }
    }
    // Deliberately NOT calling NimBLEDevice::deinit() — the stack
    // stays initialised for the process lifetime so subsequent begin()
    // calls just re-start advertising. See comment above s_stack_
    // initialized for the crash rationale. ~10 KB RAM cost is a
    // fair trade for a crash-free cancel gesture.
    active_             = false;
    pairing_state_      = PairingState::Closed;
    client_connected_   = false;
    connected_at_ms_    = 0;
    Serial.println("[ble] down (advertising stopped; stack retained)");
}

void BleService::on_pairing_control_write(uint8_t action) {
    if (pairing_state_ != PairingState::Open) return;
    switch (action) {
        case 0x01:  pairing_state_ = PairingState::Committed; break;
        case 0x02:  pairing_state_ = PairingState::Sleeping;  break;
        case 0x03:  pairing_state_ = PairingState::Aborted;   break;
        default:    /* silently ignore unknown */             break;
    }
    Serial.printf("[ble] pair_ctrl action=0x%02X -> state=%u\n",
                  static_cast<unsigned>(action),
                  static_cast<unsigned>(pairing_state_));
}

void BleService::on_client_connected() {
    client_connected_ = true;
    connected_at_ms_  = ::millis();
    const uint16_t count = (s_server ? s_server->getConnectedCount() : 0);
    Serial.printf("[ble] client connected — window paused (getConnectedCount=%u)\n",
                  (unsigned)count);
}

void BleService::on_client_disconnected() {
    uint32_t connected_for = 0;
    if (client_connected_ && pairing_state_ == PairingState::Open) {
        // Shift the window origin forward by the "connected" duration so
        // the operator gets the full remaining window back after the
        // client drops. Guards against the client-flapping case where a
        // phone connects, disconnects, connects again — the window
        // pauses/resumes rather than accumulating drift.
        connected_for = ::millis() - connected_at_ms_;
        window_started_ms_ += connected_for;
    }
    client_connected_ = false;
    Serial.printf("[ble] client disconnected — window resumed (connected_for=%lu ms, window shifted)\n",
                  (unsigned long)connected_for);
}

void BleService::on_config_write_success() {
    write_seen_ = true;
}

uint32_t BleService::seconds_remaining() const {
    if (!active_ || pairing_state_ != PairingState::Open) return 0;
    const uint32_t elapsed_ms = ::millis() - window_started_ms_;
    const uint32_t total_ms   = static_cast<uint32_t>(pair_win_s_) * 1000u;
    if (elapsed_ms >= total_ms) return 0;
    return (total_ms - elapsed_ms) / 1000u;
}

// ============================================================================
// Epic 20 B10 — Central-role scan + configure
// ============================================================================
//
// Turns the StickC (any host, really — the code is host-agnostic) into
// a BLE central so an operator can walk the fleet and reconfigure
// Lumes without a phone. Coexists cleanly with the peripheral service:
// the UX makes them mutually exclusive (Config > BLE Pair uses the
// peripheral, Config > Config Lumes uses the central), and NimBLE
// itself has no problem being both simultaneously.

namespace {

// NimBLE scan callback bridges into BleService via the singleton so
// the scan loop can populate the discovered list on the main task.
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
public:
    void onResult(NimBLEAdvertisedDevice* dev) override {
        if (!dev) return;
        // Filter to advertisers claiming the NocturNation service. The
        // adv-name-based filter alone would let random devices with a
        // clashing name string in; the service-UUID check makes it
        // deterministic.
        if (!dev->isAdvertisingService(NimBLEUUID(uuid::kService))) return;

        const char* name = dev->getName().c_str();
        uint8_t mac[6] = {};
        NimBLEAddress addr = dev->getAddress();
        // NimBLEAddress::getNative() returns a little-endian pointer;
        // copy into big-endian byte order so it matches what device_
        // info.bt_mac reports.
        const uint8_t* p = addr.getNative();
        if (p) {
            mac[0] = p[5]; mac[1] = p[4]; mac[2] = p[3];
            mac[3] = p[2]; mac[4] = p[1]; mac[5] = p[0];
        }
        // Bench 2026-09-20: capture the address TYPE too. Assuming
        // BLE_ADDR_PUBLIC in configure_lume() would target a
        // non-existent peer for any Lume advertising with a random-
        // static address, which is common — ESP32 defaults to public
        // for us, but the safer path is to echo whatever the peer
        // announced.
        ble_service().on_scan_result(mac, addr.getType(), name, dev->getRSSI());
    }
};
ScanCallbacks s_scan_callbacks;

}  // namespace

bool BleService::start_scan(uint32_t duration_ms) {
    if (scan_state_ == ScanState::Scanning) return true;

    // Reuse the same one-shot init the peripheral role uses so scan
    // works even when begin() was never called (typical: operator
    // enters Config Lumes without first entering BLE Pair).
    if (!s_stack_initialized) {
        NimBLEDevice::init("NocturNation");
        NimBLEDevice::setPower(ESP_PWR_LVL_P9);
        // Note: we're leaving the peripheral service unregistered here.
        // The first call to begin() will register it; subsequent calls
        // find s_stack_initialized true and skip re-registration.
        // For scan-only use this is fine — no one connects to us.
        s_stack_initialized = true;
    }

    discovered_count_ = 0;
    for (size_t i = 0; i < kMaxDiscovered; ++i) discovered_[i].valid = false;
    scan_state_ = ScanState::Scanning;

    NimBLEScan* scan = NimBLEDevice::getScan();
    scan->setAdvertisedDeviceCallbacks(&s_scan_callbacks, /*wantDuplicates=*/false);
    scan->setActiveScan(true);
    scan->setInterval(0x50);   // 50 ms
    scan->setWindow(0x30);     // 30 ms
    // Duration is seconds in NimBLE-Arduino; clamp to 1..30.
    uint32_t secs = (duration_ms + 999) / 1000;
    if (secs < 1)  secs = 1;
    if (secs > 30) secs = 30;
    Serial.printf("[ble] central scan for %u s\n", (unsigned)secs);
    // start(secs, cb) is non-blocking when cb is provided. We use the
    // sync-blocking variant so on_scan_complete fires on this task.
    scan->start(secs, /*is_continue=*/false);
    // NimBLE returns synchronously — mark done. The library's stop
    // will fire naturally at duration.
    on_scan_complete();
    return true;
}

void BleService::stop_scan() {
    if (scan_state_ != ScanState::Scanning) {
        scan_state_ = ScanState::Idle;
        return;
    }
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) scan->stop();
    scan_state_ = ScanState::Done;
}

void BleService::on_scan_result(const uint8_t bt_mac[6],
                                uint8_t addr_type,
                                const char* adv_name,
                                int8_t rssi) {
    // De-duplicate by MAC.
    for (size_t i = 0; i < discovered_count_; ++i) {
        if (std::memcmp(discovered_[i].bt_mac, bt_mac, 6) == 0) {
            // Refresh RSSI + adv_name in case it changed since first hit.
            discovered_[i].rssi      = rssi;
            discovered_[i].addr_type = addr_type;
            if (adv_name && adv_name[0]) {
                std::strncpy(discovered_[i].adv_name, adv_name,
                             sizeof(discovered_[i].adv_name) - 1);
                discovered_[i].adv_name[sizeof(discovered_[i].adv_name) - 1] = '\0';
            }
            return;
        }
    }
    if (discovered_count_ >= kMaxDiscovered) return;

    DiscoveredLume& slot = discovered_[discovered_count_];
    std::memcpy(slot.bt_mac, bt_mac, 6);
    slot.addr_type = addr_type;
    if (adv_name && adv_name[0]) {
        std::strncpy(slot.adv_name, adv_name, sizeof(slot.adv_name) - 1);
        slot.adv_name[sizeof(slot.adv_name) - 1] = '\0';
    } else {
        std::snprintf(slot.adv_name, sizeof(slot.adv_name),
                      "??-%02X%02X%02X",
                      static_cast<unsigned>(bt_mac[3]),
                      static_cast<unsigned>(bt_mac[4]),
                      static_cast<unsigned>(bt_mac[5]));
    }
    slot.rssi  = rssi;
    slot.valid = true;
    ++discovered_count_;
    Serial.printf("[ble] found %s type=%u rssi=%d (%u total)\n",
                  slot.adv_name, (unsigned)addr_type,
                  (int)rssi, (unsigned)discovered_count_);
}

void BleService::on_scan_complete() {
    NimBLEScan* scan = NimBLEDevice::getScan();
    if (scan) scan->clearResults();
    scan_state_ = ScanState::Done;
    Serial.printf("[ble] scan complete: %u discovered\n",
                  (unsigned)discovered_count_);
}

ConfigureResult BleService::configure_lume(const DiscoveredLume& target,
                                           const uint8_t* bag_tlv,
                                           size_t bag_len) {
    if (!bag_tlv || bag_len == 0) return ConfigureResult::WriteFailed;

    // Build the NimBLEAddress from our big-endian MAC (reverse for
    // NimBLE's native little-endian format). Use the address TYPE
    // captured from the advertisement — assuming BLE_ADDR_PUBLIC here
    // would target a non-existent peer whenever the Lume advertises
    // with a random-static address (bench 2026-09-20: connections
    // silently no-op'd on the Atom for this reason).
    uint8_t nimble_mac[6] = {
        target.bt_mac[5], target.bt_mac[4], target.bt_mac[3],
        target.bt_mac[2], target.bt_mac[1], target.bt_mac[0],
    };
    NimBLEAddress addr(nimble_mac, target.addr_type);

    // Bench 2026-09-21: ensure the scan session is fully released
    // before initiating a connection. NimBLE-Arduino's blocking-scan
    // returns when the duration expires but the host-side scan state
    // can linger for another controller cycle; a connect attempted
    // during that window silently no-ops on ESP32-to-ESP32 links.
    if (auto* scan = NimBLEDevice::getScan()) {
        scan->stop();
        scan->clearResults();
    }
    delay(100);

    NimBLEClient* client = NimBLEDevice::createClient();
    if (!client) {
        Serial.println("[ble] createClient returned null");
        return ConfigureResult::ConnectFailed;
    }
    // Explicit connection parameters. ESP32-to-ESP32 links routinely
    // fail to negotiate if we leave everything at NimBLE defaults;
    // the controller times out before the peer settles on a slot.
    // 24 * 1.25 ms = 30 ms interval, 0 latency, 2000 ms supervision.
    client->setConnectionParams(24, 24, 0, 200);
    // Bump the connect timeout to 15 s: an ESP32 peripheral coming
    // out of an active advertising cycle can take up to ~10 s to
    // acknowledge the first connect request when the phy is contended.
    client->setConnectTimeout(15);
    Serial.printf("[ble] connecting to %s (type=%u)...\n",
                  target.adv_name, (unsigned)target.addr_type);
    const uint32_t t_connect_start = ::millis();
    // Pass deleteAttribute=true so any cached GATT state from a
    // previous session is discarded — safer than reusing potentially-
    // stale attribute handles across pairing cycles.
    if (!client->connect(addr, /*deleteAttribute=*/true)) {
        Serial.printf("[ble] connect FAILED after %lu ms\n",
                      (unsigned long)(::millis() - t_connect_start));
        NimBLEDevice::deleteClient(client);
        return ConfigureResult::ConnectFailed;
    }
    Serial.printf("[ble] connected in %lu ms\n",
                  (unsigned long)(::millis() - t_connect_start));

    NimBLERemoteService* svc = client->getService(NimBLEUUID(uuid::kService));
    if (!svc) {
        Serial.println("[ble] service not found on peer");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return ConfigureResult::ServiceNotFound;
    }
    Serial.println("[ble] service resolved");

    NimBLERemoteCharacteristic* chr_cfg = svc->getCharacteristic(NimBLEUUID(uuid::kConfig));
    NimBLERemoteCharacteristic* chr_ctl = svc->getCharacteristic(NimBLEUUID(uuid::kPairingControl));
    if (!chr_cfg || !chr_ctl) {
        Serial.println("[ble] required characteristic missing on peer");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return ConfigureResult::CharacteristicNotFound;
    }

    // Write the property bag first, then the commit action. Both
    // writes-with-response so we know the peer acknowledged them.
    Serial.printf("[ble] writing config (%u bytes)...\n", (unsigned)bag_len);
    if (!chr_cfg->writeValue(bag_tlv, bag_len, /*response=*/true)) {
        Serial.println("[ble] config write FAILED");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return ConfigureResult::WriteFailed;
    }
    Serial.println("[ble] config write OK");

    uint8_t commit_action = 0x01;   // pairing_control::commit per spec §3.4
    Serial.println("[ble] committing...");
    if (!chr_ctl->writeValue(&commit_action, 1, /*response=*/true)) {
        Serial.println("[ble] commit write FAILED (config may have applied though)");
        client->disconnect();
        NimBLEDevice::deleteClient(client);
        return ConfigureResult::CommitFailed;
    }

    Serial.println("[ble] configure OK");
    client->disconnect();
    NimBLEDevice::deleteClient(client);
    return ConfigureResult::Ok;
}

#else  // !ARDUINO

// Native stubs — see B3a. Enough for higher-level code to compile
// natively; the codec is where native-testable logic lives.
bool BleService::begin(Role role, Host host, uint8_t pair_win_s) {
    role_       = role;
    host_       = host;
    pair_win_s_ = (pair_win_s == 0) ? kPairingWindowSecondsDefault : pair_win_s;
    for (uint8_t i = 0; i < 6; ++i) bt_mac_[i] = static_cast<uint8_t>(0xA0 + i);
    const int n = std::snprintf(
        adv_name_, sizeof(adv_name_),
        "NTN%02X%02X%X",
        static_cast<unsigned>(bt_mac_[3]),
        static_cast<unsigned>(bt_mac_[4]),
        static_cast<unsigned>((bt_mac_[5] >> 4) & 0x0F));
    (void)n;
    active_             = true;
    pairing_state_      = PairingState::Open;
    window_started_ms_  = 0;
    write_seen_         = false;
    return true;
}

PairingState BleService::tick() { return pairing_state_; }

void BleService::end() {
    active_        = false;
    pairing_state_ = PairingState::Closed;
}

void BleService::on_pairing_control_write(uint8_t action) {
    if (pairing_state_ != PairingState::Open) return;
    switch (action) {
        case 0x01:  pairing_state_ = PairingState::Committed; break;
        case 0x02:  pairing_state_ = PairingState::Sleeping;  break;
        case 0x03:  pairing_state_ = PairingState::Aborted;   break;
        default:    break;
    }
}

void BleService::on_config_write_success() { write_seen_ = true; }

void BleService::on_client_connected() {
    client_connected_ = true;
    connected_at_ms_  = 0;
}
void BleService::on_client_disconnected() {
    client_connected_ = false;
}

uint32_t BleService::seconds_remaining() const {
    return active_ && pairing_state_ == PairingState::Open ? pair_win_s_ : 0;
}

// Central-role stubs — enough for higher-level code to link natively.
// The blocking Bluetooth work is Arduino-only.
bool BleService::start_scan(uint32_t /*duration_ms*/) {
    scan_state_       = ScanState::Done;
    discovered_count_ = 0;
    return true;
}
void BleService::stop_scan() { scan_state_ = ScanState::Idle; }
void BleService::on_scan_result(const uint8_t /*mac*/[6],
                                uint8_t /*addr_type*/,
                                const char* /*name*/,
                                int8_t /*rssi*/) {}
void BleService::on_scan_complete() { scan_state_ = ScanState::Done; }
ConfigureResult BleService::configure_lume(const DiscoveredLume& /*target*/,
                                           const uint8_t* /*bag*/,
                                           size_t /*bag_len*/) {
    return ConfigureResult::Ok;
}

#endif  // ARDUINO

size_t BleService::advertising_name(char* buf, size_t buflen) const {
    if (!active_ || !buf || buflen == 0) return 0;
    const size_t need = std::strlen(adv_name_);
    if (buflen <= need) return 0;
    std::memcpy(buf, adv_name_, need + 1);
    return need;
}

}  // namespace ble
}  // namespace nocturnation
