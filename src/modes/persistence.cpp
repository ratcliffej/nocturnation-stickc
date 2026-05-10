// Shared NVS persistence helpers - implementations.
//
// Arduino builds use the Preferences library; native test builds use stubs
// that return defaults / no-op writes (no Preferences library available).

#include "persistence.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#endif

#include <cstring>

namespace nocturnation {
namespace modes {
namespace persistence {

bool is_persisted_runtime_mode(ModeId m) {
    return m == ModeId::AutonomousMaster
        || m == ModeId::Slave
        || m == ModeId::Config
        || m == ModeId::Test;
}

#ifdef ARDUINO
ModeId load_last_runtime_mode() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t raw = prefs.getUChar("last_mode", (uint8_t)kDefaultRuntimeMode);
    prefs.end();
    ModeId m = (ModeId)raw;
    return is_persisted_runtime_mode(m) ? m : kDefaultRuntimeMode;
}

void save_last_runtime_mode(ModeId m) {
    if (!is_persisted_runtime_mode(m)) return;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("last_mode", (uint8_t)m);
    prefs.end();
}

bool load_ir_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("ir_en", true);     // default ON
    prefs.end();
    return e;
}

void save_ir_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("ir_en", e);
    prefs.end();
}

bool load_screen_pulse_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("scr_puls_en", true);   // default ON
    prefs.end();
    return e;
}

void save_screen_pulse_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("scr_puls_en", e);
    prefs.end();
}

// ESP-NOW radio channel preferences. Master uses one of {1, 6, 11}; slave
// uses {0=Auto/scan, 1, 6, 11}. Defaults: master 1 (hobby), slave 0 (auto-
// scan with show priority). Per architecture spec §4.5: channel 1 = hobby /
// open community traffic, channel 11 = show / commercial; channel 6 is an
// advanced operator override only.
uint8_t load_master_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("mst_chan", 1);
    prefs.end();
    if (c != 1 && c != 6 && c != 11) c = 1;
    return c;
}

void save_master_channel(uint8_t c) {
    if (c != 1 && c != 6 && c != 11) c = 1;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_chan", c);
    prefs.end();
}

AudioCalibration load_calibration() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    AudioCalibration c = kCalibrationDefault;
    if (prefs.getBytesLength("cal") == sizeof(AudioCalibration)) {
        prefs.getBytes("cal", &c, sizeof(AudioCalibration));
    }
    prefs.end();
    return c;
}

void save_calibration(const AudioCalibration& c) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBytes("cal", &c, sizeof(AudioCalibration));
    prefs.end();
}
#else
ModeId           load_last_runtime_mode() { return kDefaultRuntimeMode; }
void             save_last_runtime_mode(ModeId)  {}
bool             load_ir_enabled()             { return true; }
void             save_ir_enabled(bool)         {}
bool             load_screen_pulse_enabled()    { return true; }
void             save_screen_pulse_enabled(bool) {}
uint8_t          load_master_channel()          { return 1; }
void             save_master_channel(uint8_t) {}
AudioCalibration load_calibration()       { return kCalibrationDefault; }
void             save_calibration(const AudioCalibration&) {}
#endif

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
