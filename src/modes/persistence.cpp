// Shared NVS persistence helpers - implementations.
//
// Arduino builds use the Preferences library; native test builds use stubs
// that return defaults / no-op writes (no Preferences library available).

#include "persistence.h"

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#include <esp_random.h>
#endif

#include <cstring>

namespace nocturnation {
namespace modes {
namespace persistence {

bool is_persisted_runtime_mode(ModeId m) {
    // DmxBridge is intentionally NOT persisted: per Q4 (revised
    // 2026-06-05), it's a sub-mode of Director reached via the
    // picker, not a top-level mode the boot path should remember.
    // On reboot the operator lands back in Director and explicitly
    // re-enters DMX Bridge if they want it.
    return m == ModeId::Director
        || m == ModeId::Lume
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

bool load_internal_ir_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("ir_int_en", true);    // default ON
    prefs.end();
    return e;
}

void save_internal_ir_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("ir_int_en", e);
    prefs.end();
}

bool load_external_ir_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("ir_ext_en", false);   // default OFF
    prefs.end();
    return e;
}

void save_external_ir_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("ir_ext_en", e);
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

// ESP-NOW radio channel preferences. Director uses one of {1, 6, 11}; Lume
// uses {0=Auto/scan, 1, 6, 11}. Defaults: Director 1 (hobby), Lume 0 (auto-
// scan with show priority). Per architecture spec §4.5: channel 1 = hobby /
// open community traffic, channel 11 = show / commercial; channel 6 is an
// advanced operator override only.
uint8_t load_director_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("mst_chan", 1);
    prefs.end();
    if (c != 1 && c != 6 && c != 11) c = 1;
    return c;
}

void save_director_channel(uint8_t c) {
    if (c != 1 && c != 6 && c != 11) c = 1;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_chan", c);
    prefs.end();
}

// Lume channel: 0 = auto (dual-channel scan with show priority); 1 / 6 /
// 11 = locked. Validated on read so an out-of-range value persisted by an
// older build can't push LumeMode into an invalid state.
uint8_t load_lume_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("slv_chan", 0);
    prefs.end();
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    return c;
}

void save_lume_channel(uint8_t c) {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_chan", c);
    prefs.end();
}

// Headless repeater channel: 0 = auto-scan, else 1 / 6 / 11. First-boot
// default comes from the build flag so a fleet can ship pre-pinned; the
// operator's button double-click then persists a live override.
#ifndef NOCT_REPEATER_CHANNEL
#define NOCT_REPEATER_CHANNEL 0
#endif
uint8_t load_repeater_channel() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t c = prefs.getUChar("rpt_chan", NOCT_REPEATER_CHANNEL);
    prefs.end();
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    return c;
}

void save_repeater_channel(uint8_t c) {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("rpt_chan", c);
    prefs.end();
}

bool load_lume_repeat_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    bool e = prefs.getBool("slv_repeat", false);   // default OFF
    prefs.end();
    return e;
}

void save_lume_repeat_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putBool("slv_repeat", e);
    prefs.end();
}

// Lume NocturNation group ID. Device-wide value used by LumeMode's
// receive filter. Range 0-255; operator can set anywhere in that
// range via Config.
//
// First-boot behaviour (EMF stage-team feature 2026-06-23): when the
// NVS key doesn't exist, generate a random group in [1, 3] and
// persist it. With a swarm of Lumes coming out of the box, this
// gives the LD ~3 evenly-distributed addressable zones for free -
// no need to walk the room with a phone setting each badge by hand.
// The operator can still override via the Config menu afterwards
// (writing any value, including 0 for "broadcast only").
//
// The random pick uses esp_random() so each Stick rolls
// independently; the result is persisted to NVS on first read so
// the group stays stable across power cycles.
uint8_t load_lume_group() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    uint8_t g;
    if (!prefs.isKey("slv_group")) {
        // First boot: roll [1, 3] inclusive and persist.
        g = static_cast<uint8_t>(1 + (esp_random() % 3));
        prefs.putUChar("slv_group", g);
    } else {
        g = prefs.getUChar("slv_group", 0);
    }
    prefs.end();
    return g;
}

void save_lume_group(uint8_t g) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("slv_group", g);
    prefs.end();
}

uint8_t load_strip_brightness() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t pct = prefs.getUChar("strip_bri", kDefaultStripBrightness);
    prefs.end();
    if (pct > 100) pct = 100;
    return pct;
}

void save_strip_brightness(uint8_t pct) {
    if (pct > 100) pct = 100;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("strip_bri", pct);
    prefs.end();
}

bool load_strip_enabled() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    const uint8_t v = prefs.getUChar("strip_en", 1);
    prefs.end();
    return v != 0;
}

void save_strip_enabled(bool e) {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("strip_en", e ? 1 : 0);
    prefs.end();
}

uint16_t load_strip_chain_size() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint16_t n = prefs.getUShort("strip_cnt", kDefaultStripChainSize);
    prefs.end();
    if (n == 0)  n = kDefaultStripChainSize;
    if (n > 288) n = 288;
    return n;
}

void save_strip_chain_size(uint16_t n) {
    if (n == 0)  n = kDefaultStripChainSize;
    if (n > 288) n = 288;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUShort("strip_cnt", n);
    prefs.end();
}

uint8_t load_strip_group_size() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t n = prefs.getUChar("strip_grp", kDefaultStripGroupSize);
    prefs.end();
    if (n == 0) n = 1;
    return n;
}

void save_strip_group_size(uint8_t n) {
    if (n == 0) n = 1;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("strip_grp", n);
    prefs.end();
}

void apply_strip_force_defaults() {
    if (!kStripForceDefaults) return;
    constexpr const char* kBuildTag = __DATE__ " " __TIME__;
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    char stored[40] = { 0 };
    prefs.getString("strip_btag", stored, sizeof(stored));
    if (std::strcmp(stored, kBuildTag) == 0) {
        prefs.end();
        return;     // this build's defaults already applied
    }
    prefs.putUChar("strip_en",   kDefaultStripEnabled ? 1 : 0);
    prefs.putUChar("strip_bri",  kDefaultStripBrightness);
    prefs.putUChar("strip_grp",  kDefaultStripGroupSize);
    prefs.putUShort("strip_cnt", kDefaultStripChainSize);
    prefs.putString("strip_btag", kBuildTag);
    prefs.end();
}

uint8_t load_director_source_id() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/true);
    uint8_t id = prefs.getUChar("mst_src_id", 0);
    prefs.end();
    // Defensive validation. The key is normally guaranteed-present and
    // guaranteed-in-range by migrate_legacy_nvs_keys, but an older
    // firmware build or a corrupt NVS entry could write something
    // outside [0x00, 0x3F]. Clamp into the community range so callers
    // can rely on the return value without re-validating.
    if (id > 0x3F) id = 0;
    return id;
}

void save_director_source_id(uint8_t id) {
    if (id > 0x3F) id = 0;   // community range only
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_src_id", id);
    prefs.end();
}

uint8_t load_director_perf_source_id() {
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    uint8_t id = prefs.getUChar("mst_pid", 0);
    // Treat absent OR out-of-range as "first install" and roll a
    // fresh value in [0x40, 0xFE] (191 slots). Persist immediately
    // so subsequent boots return the same id - giving a Director a
    // stable identity that Lumes lock to consistently.
    if (id < 0x40 || id > 0xFE) {
        id = static_cast<uint8_t>(0x40 + (esp_random() % 191));
        prefs.putUChar("mst_pid", id);
    }
    prefs.end();
    return id;
}

void save_director_perf_source_id(uint8_t id) {
    if (id < 0x40 || id > 0xFE) return;   // defensive; out-of-range silently dropped
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_pid", id);
    prefs.end();
}

uint8_t reroll_director_perf_source_id() {
    const uint8_t id = static_cast<uint8_t>(0x40 + (esp_random() % 191));
    Preferences prefs;
    prefs.begin("noct", /*readOnly=*/false);
    prefs.putUChar("mst_pid", id);
    prefs.end();
    return id;
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
    // framework replaces the Visualisation framework as the Director-side
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

    // First-boot slv_group assignment. If slv_group has never been
    // written (fresh flash or post-factory-reset), pick a random value
    // in {1, 2, 3} and persist it. Three groups are the canonical drum
    // split DynamicShow routes kick / snare / hi-hat to, so a fleet of
    // freshly-flashed Sticks naturally distributes across per-drum
    // addressing on the operator's first power-on. Once persisted, the
    // value is stable across reboots and only changes when the operator
    // edits it via Config > Group. An operator deliberately setting
    // slv_group = 0 is honoured and not retro-randomised.
    if (!prefs.isKey("slv_group")) {
        const uint8_t g = (esp_random() % 3) + 1;
        prefs.putUChar("slv_group", g);
    }

    // First-boot mst_src_id assignment (Epic 5.5 B3). If mst_src_id has
    // never been written, pick a random value in the community range
    // [0x00, 0x3F] and persist it. Stable across reboots so a returning
    // Lume locks back onto the same Director on channel 1. Out-of-range
    // values (legacy firmware, NVS corruption) are retroactively
    // corrected by re-rolling.
    {
        const bool present = prefs.isKey("mst_src_id");
        const bool valid   = present && prefs.getUChar("mst_src_id", 0xFF) <= 0x3F;
        if (!valid) {
            const uint8_t id = static_cast<uint8_t>(esp_random() & 0x3F);
            prefs.putUChar("mst_src_id", id);
        }
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
// Native test stubs. The Lume-channel / repeater values are held in
// process-static memory so test fixtures can seed values via save_*
// and exercise LumeMode's enter() against the same load_* helpers
// that the firmware path uses. The legacy-key migration delegates to
// a process-static "noct/slv_ir_grp" stand-in that tests seed via
// the helper at the bottom of this block.
namespace {
uint8_t s_native_lume_channel    = 0;
uint8_t s_native_repeater_channel = 0;
bool    s_native_lume_repeat_en  = false;
uint8_t s_native_lume_group        = 0;
bool    s_native_lume_group_set    = false;   // tracks "has slv_group been written" (the isKey() analogue)
uint8_t s_native_first_boot_rng   = 2;       // deterministic stand-in for esp_random() % 3 + 1
uint8_t s_native_director_src_id     = 0;
bool    s_native_director_src_id_set = false;
uint8_t s_native_first_boot_director_src_id_rng = 0x05;  // deterministic stand-in for esp_random() & 0x3F
// Performance-range Director id (channel 11). Stable across boots
// once rolled; the boot-time RNG hook lets tests force a known
// first-install value the same way s_native_first_boot_director_src_id_rng
// does for the community-range counterpart.
uint8_t s_native_director_perf_src_id     = 0;
bool    s_native_director_perf_src_id_set = false;
uint8_t s_native_first_boot_director_perf_src_id_rng = 0x4F;
// Re-roll RNG hook - separate from the first-install hook so tests
// can drive the "operator pressed Re-roll on the Config menu" path
// with a known new value.
uint8_t s_native_reroll_director_perf_src_id_rng     = 0x7A;
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
bool             load_internal_ir_enabled()    { return true; }   // default ON
void             save_internal_ir_enabled(bool) {}
bool             load_external_ir_enabled()    { return false; }  // default OFF
void             save_external_ir_enabled(bool) {}
bool             load_screen_pulse_enabled()    { return true; }
void             save_screen_pulse_enabled(bool) {}
uint8_t          load_director_channel()          { return 1; }
void             save_director_channel(uint8_t) {}
uint8_t          load_lume_channel()           { return s_native_lume_channel; }
void             save_lume_channel(uint8_t c)  {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    s_native_lume_channel = c;
}
uint8_t          load_repeater_channel()       { return s_native_repeater_channel; }
void             save_repeater_channel(uint8_t c) {
    if (c != 0 && c != 1 && c != 6 && c != 11) c = 0;
    s_native_repeater_channel = c;
}
bool             load_lume_repeat_enabled()           { return s_native_lume_repeat_en; }
void             save_lume_repeat_enabled(bool e)     { s_native_lume_repeat_en = e; }
uint8_t          load_lume_group()                       { return s_native_lume_group; }
void             save_lume_group(uint8_t g)              {
    s_native_lume_group     = g;
    s_native_lume_group_set = true;
}

namespace {
uint8_t  s_native_strip_brightness = kDefaultStripBrightness;
bool     s_native_strip_enabled    = true;
uint16_t s_native_strip_chain_size = kDefaultStripChainSize;
uint8_t  s_native_strip_group_size = kDefaultStripGroupSize;
}  // namespace
uint8_t          load_strip_brightness()                 { return s_native_strip_brightness; }
void             save_strip_brightness(uint8_t pct)      {
    if (pct > 100) pct = 100;
    s_native_strip_brightness = pct;
}
bool             load_strip_enabled()                    { return s_native_strip_enabled; }
void             save_strip_enabled(bool e)              { s_native_strip_enabled = e; }
uint16_t         load_strip_chain_size()                 { return s_native_strip_chain_size; }
void             save_strip_chain_size(uint16_t n)       {
    if (n == 0)  n = kDefaultStripChainSize;
    if (n > 288) n = 288;
    s_native_strip_chain_size = n;
}
uint8_t          load_strip_group_size()                 { return s_native_strip_group_size; }
void             save_strip_group_size(uint8_t n)        {
    if (n == 0) n = 1;
    s_native_strip_group_size = n;
}
void apply_strip_force_defaults()                        {
    if (!kStripForceDefaults) return;
    s_native_strip_enabled    = kDefaultStripEnabled;
    s_native_strip_brightness = kDefaultStripBrightness;
    s_native_strip_group_size = kDefaultStripGroupSize;
    s_native_strip_chain_size = kDefaultStripChainSize;
}

uint8_t load_director_source_id() {
    // Mirror the Arduino branch's defensive clamp: an out-of-range
    // persisted value is treated as "no valid value" and returns 0.
    if (!s_native_director_src_id_set) return 0;
    return s_native_director_src_id <= 0x3F ? s_native_director_src_id : 0;
}

void save_director_source_id(uint8_t id) {
    if (id > 0x3F) id = 0;
    s_native_director_src_id     = id;
    s_native_director_src_id_set = true;
}

uint8_t load_director_perf_source_id() {
    // First-install path: if absent or out-of-range, roll via the
    // deterministic native RNG hook, persist, return.
    const bool present = s_native_director_perf_src_id_set
                         && s_native_director_perf_src_id >= 0x40
                         && s_native_director_perf_src_id <= 0xFE;
    if (!present) {
        s_native_director_perf_src_id     = s_native_first_boot_director_perf_src_id_rng;
        s_native_director_perf_src_id_set = true;
    }
    return s_native_director_perf_src_id;
}

void save_director_perf_source_id(uint8_t id) {
    if (id < 0x40 || id > 0xFE) return;
    s_native_director_perf_src_id     = id;
    s_native_director_perf_src_id_set = true;
}

uint8_t reroll_director_perf_source_id() {
    s_native_director_perf_src_id     = s_native_reroll_director_perf_src_id_rng;
    s_native_director_perf_src_id_set = true;
    return s_native_director_perf_src_id;
}

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

    // First-boot slv_group assignment. Mirrors the Arduino path: if the
    // slv_group key has never been written, pick a value in {1, 2, 3}
    // and persist it. Native uses the seam-controlled
    // s_native_first_boot_rng instead of esp_random() so tests can
    // assert a deterministic outcome.
    if (!s_native_lume_group_set) {
        s_native_lume_group     = s_native_first_boot_rng;
        s_native_lume_group_set = true;
    }

    // First-boot mst_src_id assignment. Mirrors the Arduino branch: if
    // no value has been written, or the persisted value is outside the
    // community range, pick from the deterministic test seam (instead
    // of esp_random) so the test outcome is predictable.
    {
        const bool valid = s_native_director_src_id_set &&
                           s_native_director_src_id <= 0x3F;
        if (!valid) {
            s_native_director_src_id     = s_native_first_boot_director_src_id_rng;
            s_native_director_src_id_set = true;
        }
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
void set_first_boot_rng(uint8_t g_in_1_3) {
    s_native_first_boot_rng = g_in_1_3;
}
void set_first_boot_director_src_id_rng(uint8_t id_in_community_range) {
    // Clamp defensively; tests pass values in range but a stray caller
    // shouldn't silently get a Performance-range value here.
    s_native_first_boot_director_src_id_rng =
        (id_in_community_range <= 0x3F) ? id_in_community_range : 0;
}
void plant_raw_director_src_id(uint8_t id) {
    // Bypass save_'s clamp to simulate a corrupted-NVS / older-firmware
    // state where mst_src_id holds a value outside the community range.
    s_native_director_src_id     = id;
    s_native_director_src_id_set = true;
}
void set_first_boot_director_perf_src_id_rng(uint8_t id_in_perf_range) {
    s_native_first_boot_director_perf_src_id_rng =
        (id_in_perf_range >= 0x40 && id_in_perf_range <= 0xFE) ? id_in_perf_range : 0x40;
}
void set_reroll_director_perf_src_id_rng(uint8_t id_in_perf_range) {
    s_native_reroll_director_perf_src_id_rng =
        (id_in_perf_range >= 0x40 && id_in_perf_range <= 0xFE) ? id_in_perf_range : 0x40;
}
void plant_raw_director_perf_src_id(uint8_t id) {
    s_native_director_perf_src_id     = id;
    s_native_director_perf_src_id_set = true;
}
void clear_native_persistence() {
    s_native_lume_channel             = 0;
    s_native_lume_repeat_en           = false;
    s_native_lume_group                 = 0;
    s_native_lume_group_set             = false;
    s_native_first_boot_rng            = 2;
    s_native_director_src_id            = 0;
    s_native_director_src_id_set        = false;
    s_native_first_boot_director_src_id_rng = 0x05;
    s_native_director_perf_src_id      = 0;
    s_native_director_perf_src_id_set  = false;
    s_native_first_boot_director_perf_src_id_rng = 0x4F;
    s_native_reroll_director_perf_src_id_rng     = 0x7A;
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
