// NocturNation BLE GATT service implementation (Epic 20 B3a).
//
// v0x01 skeleton: NimBLE stack lifecycle + service registration with
// empty characteristics. Characteristic reads/writes land in B3c.
//
// Non-Arduino builds skip the NimBLE bits entirely (guarded on ARDUINO):
// begin/end/is_active still exist but no-op, so higher-level code that
// includes ble_service.h can compile natively without pulling in the
// BLE stack. The property-bag TLV codec (B3b) is where the testable
// logic will live.

#include "ble_service.h"

#include <cstdio>
#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <esp_bt_device.h>
#endif

namespace nocturnation {
namespace ble {

namespace {
BleService s_instance;
}  // namespace

BleService& ble_service() { return s_instance; }

const char* BleService::role_label(Role r) {
    switch (r) {
        case Role::Director: return "Director";
        case Role::Lume:     return "Lume";
    }
    return "Unknown";
}

#ifdef ARDUINO

namespace {
// Compose "NCTN-<Role>-XXYYZZ" into buf; returns chars written.
size_t compose_adv_name(char* buf, size_t buflen,
                        const char* role_label,
                        const uint8_t* mac) {
    if (buflen < 21 || !buf || !role_label || !mac) return 0;
    const int n = std::snprintf(
        buf, buflen,
        "NCTN-%s-%02X%02X%02X",
        role_label,
        static_cast<unsigned>(mac[3]),
        static_cast<unsigned>(mac[4]),
        static_cast<unsigned>(mac[5]));
    return (n < 0) ? 0 : static_cast<size_t>(n);
}

// Fetch BT MAC via ESP-IDF. Returns true if the address was populated,
// false if the BT stack wasn't initialised yet (in which case we fall
// back to zeros and log a warning - non-fatal for advertising).
bool fetch_bt_mac(uint8_t out[6]) {
    const uint8_t* addr = esp_bt_dev_get_address();
    if (!addr) return false;
    std::memcpy(out, addr, 6);
    return true;
}
}  // namespace

bool BleService::begin(Role role, Host host, uint8_t pair_win_s) {
    if (active_) return true;

    role_       = role;
    host_       = host;
    pair_win_s_ = (pair_win_s == 0) ? kPairingWindowSecondsDefault : pair_win_s;

    // NimBLE init: safe to call multiple times across begin/end cycles
    // per the h2zero/NimBLE-Arduino API contract. Init string is used
    // only as a placeholder device name — the advertising name below
    // overrides it.
    NimBLEDevice::init("NocturNation");

    // Pin the BT MAC to the hardware address. NimBLE defaults to public
    // address on ESP32 (which IS the hardware address) but be explicit
    // per Docs/manuals/ble-service.md §10 requirement.
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  // +9 dBm — max, for pairing range.

    if (!fetch_bt_mac(bt_mac_)) {
        Serial.println("[ble] warn: esp_bt_dev_get_address returned null; adv name will have zero suffix");
        std::memset(bt_mac_, 0, sizeof(bt_mac_));
    }
    compose_adv_name(adv_name_, sizeof(adv_name_), role_label(role_), bt_mac_);

    // Server + service registration. Characteristics are empty stubs in
    // v0x01 B3a; the callbacks that populate them land in B3c.
    NimBLEServer* server = NimBLEDevice::createServer();
    NimBLEService* svc   = server->createService(uuid::kService);

    // Declare all five mandatory + one reserved characteristic with the
    // access flags they will need in B3c. Read on the read-only ones so
    // a scanner can enumerate them; the read callback returns empty for
    // now (NimBLE will send a zero-length response).
    svc->createCharacteristic(uuid::kDeviceInfo,
                              NIMBLE_PROPERTY::READ);
    svc->createCharacteristic(uuid::kConfig,
                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    svc->createCharacteristic(uuid::kStatus,
                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
    svc->createCharacteristic(uuid::kPairingControl,
                              NIMBLE_PROPERTY::WRITE);
    svc->createCharacteristic(uuid::kShowPassthrough,
                              NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);

    svc->start();

    // Advertising with the service UUID + composed name.
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(uuid::kService);
    adv->setName(adv_name_);
    adv->setScanResponse(false);
    adv->setMinInterval(0x20);  // 20 ms — snappy for a short window
    adv->setMaxInterval(0x40);  // 40 ms

    if (!adv->start()) {
        Serial.println("[ble] advertising start FAILED");
        NimBLEDevice::deinit(false);
        return false;
    }

    active_ = true;
    Serial.printf("[ble] up: name=%s uuid=%s\n", adv_name_, uuid::kService);
    return true;
}

void BleService::end() {
    if (!active_) return;
    NimBLEDevice::getAdvertising()->stop();
    NimBLEDevice::deinit(/*clearAll=*/true);  // frees server + services
    active_ = false;
    Serial.println("[ble] down");
}

#else  // !ARDUINO

// Native stubs: keep begin/end/is_active honest for any code that includes
// this header, but do not touch a BLE stack. The TLV codec (B3b) is where
// native tests do actual work.
bool BleService::begin(Role role, Host host, uint8_t pair_win_s) {
    role_       = role;
    host_       = host;
    pair_win_s_ = (pair_win_s == 0) ? kPairingWindowSecondsDefault : pair_win_s;
    // Stub MAC keeps advertising_name() deterministic for tests that
    // compare the composed string.
    for (uint8_t i = 0; i < 6; ++i) bt_mac_[i] = static_cast<uint8_t>(0xA0 + i);
    const int n = std::snprintf(
        adv_name_, sizeof(adv_name_),
        "NCTN-%s-%02X%02X%02X",
        role_label(role_),
        static_cast<unsigned>(bt_mac_[3]),
        static_cast<unsigned>(bt_mac_[4]),
        static_cast<unsigned>(bt_mac_[5]));
    (void)n;
    active_ = true;
    return true;
}

void BleService::end() {
    active_ = false;
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
