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

// Slave channel: 0 = auto (dual-channel scan with show priority); 1 / 6 /
// 11 = locked. Validated on read so an out-of-range value persisted by an
// older build can't push SlaveMode into an invalid state.
uint8_t load_slave_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("slv_chan", 0);
    prefs.end();
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    return c;
}

void save_slave_channel(uint8_t c) {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_chan", c);
    prefs.end();
}

bool load_slave_repeat_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("slv_repeat", false);   // default OFF
    prefs.end();
    return e;
}

void save_slave_repeat_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("slv_repeat", e);
    prefs.end();
}

// Slave NocturNation group ID. Device-wide value used by SlaveMode's
// receive filter. Range 0-255; default 0 means "respond to everything".
uint8_t load_slv_group() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t g = prefs.getUChar("slv_group", 0);
    prefs.end();
    return g;
}

void save_slv_group(uint8_t g) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_group", g);
    prefs.end();
}

// Active visualisation id (legacy, pre-Epic-4.7). Retained for back-
// compat reads during migration; new code uses load_active_show_id.
// Default is "beat-pulse" - the canonical pre-Block-1 vis - so a
// freshly-initialised buffer maps cleanly to "simple-beat" via the
// Block 1 migration if it leaks into a read before migrate runs.
namespace {
constexpr size_t kActiveVisBufSize = 16;
char             s_active_vis_buf[kActiveVisBufSize] = "beat-pulse";
}

const char* load_active_vis_id() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    size_t n = prefs.getString("active_vis",
                                s_active_vis_buf,
                                kActiveVisBufSize);
    prefs.end();
    if (n == 0) {
        std::strncpy(s_active_vis_buf, "beat-pulse", kActiveVisBufSize);
        s_active_vis_buf[kActiveVisBufSize - 1] = '\0';
    }
    return s_active_vis_buf;
}

void save_active_vis_id(const char* id) {
    if (!id) return;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putString("active_vis", id);
    prefs.end();
    std::strncpy(s_active_vis_buf, id, kActiveVisBufSize);
    s_active_vis_buf[kActiveVisBufSize - 1] = '\0';
}

// Active Show id (Epic 4.7 Block 1). Replaces active_vis. Stored as a
// string under "noct/active_show"; 16-byte read buffer is comfortable
// for the 12-char id() cap. Default is "simple-beat" because that is
// the only Show registered in Block 1 and the canonical fallback when
// a saved id no longer resolves.
namespace {
constexpr size_t kActiveShowBufSize = 16;
char             s_active_show_buf[kActiveShowBufSize] = "simple-beat";
}

const char* load_active_show_id() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    size_t n = prefs.getString("active_show",
                                s_active_show_buf,
                                kActiveShowBufSize);
    prefs.end();
    if (n == 0) {
        std::strncpy(s_active_show_buf, "simple-beat", kActiveShowBufSize);
        s_active_show_buf[kActiveShowBufSize - 1] = '\0';
    }
    return s_active_show_buf;
}

void save_active_show_id(const char* id) {
    if (!id) return;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putString("active_show", id);
    prefs.end();
    std::strncpy(s_active_show_buf, id, kActiveShowBufSize);
    s_active_show_buf[kActiveShowBufSize - 1] = '\0';
}

void migrate_legacy_nvs_keys() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);

    // slv_ir_grp - legacy key from pre-Epic-4.65 firmware. Originally
    // moved to PixMobIrBinding's "group" property in Epic 4.6 Block 9;
    // that property itself was dropped in the post-Epic-4.65 cleanup
    // because the binding became a pure relay (uses inbound
    // target_group, not a per-binding fallback). The legacy key is now
    // just consumed and discarded so a stale value doesn't shadow
    // future settings. Operator factory-resets via ConfigMode > System
    // > Factory Reset, which prefs.clear()s the whole "noct" namespace.
    if (prefs.isKey("slv_ir_grp")) {
        prefs.remove("slv_ir_grp");
    }

    // active_vis -> active_show (Epic 4.7 Block 1). The Show plug-in
    // framework replaces the Visualisation framework as the master-side
    // performance unit. The canonical pre-Block-1 vis ids
    // ("beat-pulse", "spectrum-bars") both map to "simple-beat" - the
    // Block 1 Show that preserves BeatPulse's behaviour. Custom or
    // unknown vis ids pass through unchanged in case a contributor's
    // build registered a Show with the same id. If active_show is
    // already set we don't overwrite (operator may have made an
    // explicit choice post-upgrade).
    if (prefs.isKey("active_vis")) {
        char buf[16] = {0};
        prefs.getString("active_vis", buf, sizeof(buf));
        if (buf[0] != '\0' && !prefs.isKey("active_show")) {
            const bool legacy_default =
                std::strcmp(buf, "beat-pulse")    == 0 ||
                std::strcmp(buf, "spectrum-bars") == 0;
            const char* mapped = legacy_default ? "simple-beat" : buf;
            prefs.putString("active_show", mapped);
        }
        prefs.remove("active_vis");
    }

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
// Native test stubs. The slave-channel / repeater values are held in
// process-static memory so test fixtures can seed values via save_*
// and exercise SlaveMode's enter() against the same load_* helpers
// that the firmware path uses. The legacy-key migration delegates to
// a process-static "noct/slv_ir_grp" stand-in that tests seed via
// the helper at the bottom of this block.
namespace {
uint8_t s_native_slave_channel    = 0;
bool    s_native_slave_repeat_en  = false;
uint8_t s_native_slv_group        = 0;
bool    s_native_legacy_slv_ir_grp_present = false;
uint8_t s_native_legacy_slv_ir_grp_value   = 0;
constexpr size_t kActiveVisBufSize = 16;
char    s_native_active_vis[kActiveVisBufSize] = "beat-pulse";
constexpr size_t kActiveShowBufSize = 16;
char    s_native_active_show[kActiveShowBufSize] = "simple-beat";
bool    s_native_legacy_active_vis_present = false;
}  // namespace

ModeId           load_last_runtime_mode() { return kDefaultRuntimeMode; }
void             save_last_runtime_mode(ModeId)  {}
bool             load_ir_enabled()             { return true; }
void             save_ir_enabled(bool)         {}
bool             load_screen_pulse_enabled()    { return true; }
void             save_screen_pulse_enabled(bool) {}
uint8_t          load_master_channel()          { return 1; }
void             save_master_channel(uint8_t) {}
uint8_t          load_slave_channel()           { return s_native_slave_channel; }
void             save_slave_channel(uint8_t c)  {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    s_native_slave_channel = c;
}
bool             load_slave_repeat_enabled()           { return s_native_slave_repeat_en; }
void             save_slave_repeat_enabled(bool e)     { s_native_slave_repeat_en = e; }
uint8_t          load_slv_group()                       { return s_native_slv_group; }
void             save_slv_group(uint8_t g)              { s_native_slv_group = g; }

const char* load_active_vis_id() {
    return s_native_active_vis;
}

void save_active_vis_id(const char* id) {
    if (!id) return;
    std::strncpy(s_native_active_vis, id, kActiveVisBufSize);
    s_native_active_vis[kActiveVisBufSize - 1] = '\0';
}

const char* load_active_show_id() {
    return s_native_active_show;
}

void save_active_show_id(const char* id) {
    if (!id) return;
    std::strncpy(s_native_active_show, id, kActiveShowBufSize);
    s_native_active_show[kActiveShowBufSize - 1] = '\0';
}

void migrate_legacy_nvs_keys() {
    // Consume the legacy slv_ir_grp without writing it anywhere - the
    // relay PixMobIrBinding no longer has a per-binding fallback group.
    s_native_legacy_slv_ir_grp_present = false;
    s_native_legacy_slv_ir_grp_value   = 0;

    // active_vis -> active_show. Only fires when the test seam has
    // explicitly seeded the legacy key via seed_legacy_active_vis.
    // Production builds use the Arduino path above; this native branch
    // is exercised by test_modes / persistence-migration tests.
    if (s_native_legacy_active_vis_present) {
        const bool legacy_default =
            std::strcmp(s_native_active_vis, "beat-pulse")    == 0 ||
            std::strcmp(s_native_active_vis, "spectrum-bars") == 0;
        const char* mapped = legacy_default ? "simple-beat" : s_native_active_vis;
        std::strncpy(s_native_active_show, mapped, kActiveShowBufSize);
        s_native_active_show[kActiveShowBufSize - 1] = '\0';
        s_native_legacy_active_vis_present = false;
    }
}

namespace test_seam {
void seed_legacy_slv_ir_grp(uint8_t g) {
    s_native_legacy_slv_ir_grp_present = true;
    s_native_legacy_slv_ir_grp_value   = g;
}
void seed_legacy_active_vis(const char* id) {
    if (!id) return;
    std::strncpy(s_native_active_vis, id, kActiveVisBufSize);
    s_native_active_vis[kActiveVisBufSize - 1] = '\0';
    s_native_legacy_active_vis_present = true;
}
void clear_native_persistence() {
    s_native_slave_channel             = 0;
    s_native_slave_repeat_en           = false;
    s_native_slv_group                 = 0;
    s_native_legacy_slv_ir_grp_present = false;
    s_native_legacy_slv_ir_grp_value   = 0;
    std::strncpy(s_native_active_vis, "beat-pulse", kActiveVisBufSize);
    s_native_active_vis[kActiveVisBufSize - 1] = '\0';
    std::strncpy(s_native_active_show, "simple-beat", kActiveShowBufSize);
    s_native_active_show[kActiveShowBufSize - 1] = '\0';
    s_native_legacy_active_vis_present = false;
}
}  // namespace test_seam

AudioCalibration load_calibration()       { return kCalibrationDefault; }
void             save_calibration(const AudioCalibration&) {}
#endif

}  // namespace persistence
}  // namespace modes
}  // namespace nocturnation
