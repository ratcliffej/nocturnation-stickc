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

const char* BleService::role_label(Role r) {
    switch (r) {
        case Role::Director: return "Director";
        case Role::Lume:     return "Lume";
    }
    return "Unknown";
}

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
size_t compose_adv_name(char* buf, size_t buflen,
                        const char* role_label,
                        const uint8_t* mac) {
    if (buflen < 21 || !buf || !role_label || !mac) return 0;
    // Try friendly_name first. Non-empty: use verbatim (already clamped
    // to 20 bytes by save_friendly_name).
    char friendly[24] = {};
    const size_t fn_len = modes::persistence::load_friendly_name(friendly, sizeof(friendly));
    if (fn_len > 0) {
        std::memcpy(buf, friendly, fn_len);
        buf[fn_len] = '\0';
        return fn_len;
    }
    const int n = std::snprintf(
        buf, buflen,
        "NCTN-%s-%02X%02X%02X",
        role_label,
        static_cast<unsigned>(mac[3]),
        static_cast<unsigned>(mac[4]),
        static_cast<unsigned>(mac[5]));
    return (n < 0) ? 0 : static_cast<size_t>(n);
}

bool fetch_bt_mac(uint8_t out[6]) {
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
        // pair_win_s, friendly_name.
        enc.add_u8 (key::kGroup,
                    modes::persistence::load_lume_group());
        enc.add_u8 (key::kLedPower,
                    modes::persistence::load_strip_brightness());
        enc.add_u8 (key::kChannelPref,
                    modes::persistence::load_lume_channel());
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

}  // namespace

bool BleService::begin(Role role, Host host, uint8_t pair_win_s) {
    if (active_) return true;

    role_       = role;
    host_       = host;
    pair_win_s_ = (pair_win_s == 0) ? kPairingWindowSecondsDefault : pair_win_s;
    if (pair_win_s_ < 5)   pair_win_s_ = 5;

    NimBLEDevice::init("NocturNation");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);

    if (!fetch_bt_mac(bt_mac_)) {
        Serial.println("[ble] warn: esp_bt_dev_get_address returned null; adv name will have zero suffix");
        std::memset(bt_mac_, 0, sizeof(bt_mac_));
    }
    compose_adv_name(adv_name_, sizeof(adv_name_), role_label(role_), bt_mac_);

    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* svc   = server->createService(uuid::kService);

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
    adv->addServiceUUID(uuid::kService);
    adv->setName(adv_name_);
    adv->setScanResponse(false);
    adv->setMinInterval(0x20);
    adv->setMaxInterval(0x40);

    if (!adv->start()) {
        Serial.println("[ble] advertising start FAILED");
        NimBLEDevice::deinit(false);
        return false;
    }

    active_             = true;
    pairing_state_      = PairingState::Open;
    window_started_ms_  = ::millis();
    write_seen_         = false;
    Serial.printf("[ble] up: name=%s uuid=%s window=%us\n",
                  adv_name_, uuid::kService, (unsigned)pair_win_s_);
    return true;
}

PairingState BleService::tick() {
    if (!active_ || pairing_state_ == PairingState::Closed) {
        return pairing_state_;
    }
    // If the client signalled commit/commit_and_sleep/abort, keep the
    // state as-is; the caller's UI acts on it. The Open→timeout path
    // auto-closes to Aborted so a walk-away operator doesn't leave a
    // stray advertising session running.
    if (pairing_state_ == PairingState::Open) {
        const uint32_t elapsed_ms = ::millis() - window_started_ms_;
        if (elapsed_ms >= static_cast<uint32_t>(pair_win_s_) * 1000u) {
            pairing_state_ = PairingState::Aborted;
        }
    }
    return pairing_state_;
}

void BleService::end() {
    if (!active_) return;
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(/*clearAll=*/true);
    active_             = false;
    pairing_state_      = PairingState::Closed;
    s_char_device_info  = nullptr;
    s_char_config       = nullptr;
    s_char_status       = nullptr;
    s_char_pair_ctrl    = nullptr;
    s_char_show_pt      = nullptr;
    Serial.println("[ble] down");
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
        "NCTN-%s-%02X%02X%02X",
        role_label(role_),
        static_cast<unsigned>(bt_mac_[3]),
        static_cast<unsigned>(bt_mac_[4]),
        static_cast<unsigned>(bt_mac_[5]));
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

uint32_t BleService::seconds_remaining() const {
    return active_ && pairing_state_ == PairingState::Open ? pair_win_s_ : 0;
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
