// PropertyBag implementation.
//
// Two builds in one file via #ifdef ARDUINO:
//   - Arduino: backed by ESP-IDF Preferences (NVS). Per-bag namespace
//     "nv_<id>" / "nb_<id>". Small fixed-size cache (kCacheSize entries)
//     in front of NVS to absorb repeated get() within a frame.
//   - Native: in-memory only, std::vector<Entry>. clear_for_tests() empties
//     a static "all bags" registry so tests start clean.
//
// Type validation, default fallback, and bounds clamping live here -
// independent of which backend is in use.

#include "plugins/property_bag.h"

#include <cstring>

#ifdef ARDUINO
#include <Arduino.h>
#include <Preferences.h>
#else
#include <vector>
#endif

namespace nocturnation {
namespace plugins {

// -----------------------------------------------------------------------------
// Schema lookup + validation/clamping helpers (build-independent)
// -----------------------------------------------------------------------------

namespace {

const PropertyDef* find_def(const Plugin& plugin, const char* key) {
    if (!key) return nullptr;
    auto schema = plugin.properties();
    for (size_t i = 0; i < schema.size; ++i) {
        const PropertyDef& def = schema.data[i];
        if (def.key && std::strcmp(def.key, key) == 0) return &def;
    }
    return nullptr;
}

// Clamp `value` to [def.min_value, def.max_value] for U8/U16/Enum types.
// Bool/Colour pass through unchanged.
PropertyValue clamp_to_bounds(const PropertyDef& def, PropertyValue value) {
    switch (def.type) {
        case PropertyType::U8: {
            uint8_t v   = value.as_u8();
            uint8_t lo  = def.min_value.as_u8();
            uint8_t hi  = def.max_value.as_u8();
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return PropertyValue::from_u8(v);
        }
        case PropertyType::U16: {
            uint16_t v   = value.as_u16();
            uint16_t lo  = def.min_value.as_u16();
            uint16_t hi  = def.max_value.as_u16();
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return PropertyValue::from_u16(v);
        }
        case PropertyType::Enum: {
            uint8_t v   = value.as_enum();
            uint8_t lo  = def.min_value.as_enum();
            uint8_t hi  = def.max_value.as_enum();
            if (v < lo) v = lo;
            if (v > hi) v = hi;
            return PropertyValue::from_enum(v);
        }
        case PropertyType::Bool:
        case PropertyType::Colour:
        default:
            return value;
    }
}

// Compose NVS namespace from plugin kind + id. Truncates id to fit
// 12 chars (NVS namespaces max out at 15; the prefix takes 3).
// Writes into `out` which must be at least 16 chars.
void compose_namespace(const Plugin& plugin, char* out, size_t out_len) {
    const char* prefix = (plugin.kind() == PluginKind::Visualisation) ? "nv_" : "nb_";
    const char* id     = plugin.id();
    // Defensive: empty id falls back to "unknown" so we never produce "nv_".
    if (!id || !*id) id = "unknown";
    size_t i = 0;
    while (i < 3 && i < out_len - 1) {
        out[i] = prefix[i];
        ++i;
    }
    size_t j = 0;
    while (id[j] && i < out_len - 1 && j < 12) {
        out[i++] = id[j++];
    }
    out[i] = '\0';
}

}  // namespace

#ifdef ARDUINO

// -----------------------------------------------------------------------------
// Arduino implementation: NVS-backed with a small in-RAM cache
// -----------------------------------------------------------------------------

namespace {

// Cache size: typical plugins declare ~3-6 properties. 8 covers comfortably
// without bloating per-bag RAM. Plugins with more still work; reads beyond
// the cache miss into NVS more often. Documented behaviour.
constexpr size_t kCacheSize = 8;

struct CacheEntry {
    bool          valid = false;
    char          key[16] = {};
    PropertyValue value;
};

struct ArduinoImpl {
    char       ns_name[16] = {};
    CacheEntry cache[kCacheSize];

    int find_cache_slot(const char* key) const {
        for (size_t i = 0; i < kCacheSize; ++i) {
            if (cache[i].valid && std::strncmp(cache[i].key, key, 16) == 0) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    int free_cache_slot() const {
        for (size_t i = 0; i < kCacheSize; ++i) {
            if (!cache[i].valid) return static_cast<int>(i);
        }
        // Cache full: simple eviction - overwrite slot 0.
        return 0;
    }

    void cache_put(const char* key, PropertyValue value) {
        int slot = find_cache_slot(key);
        if (slot < 0) slot = free_cache_slot();
        cache[slot].valid = true;
        std::strncpy(cache[slot].key, key, 15);
        cache[slot].key[15] = '\0';
        cache[slot].value = value;
    }
};

PropertyValue read_from_nvs(const char* ns_name, const PropertyDef& def) {
    Preferences prefs;
    if (!prefs.begin(ns_name, /*readOnly=*/true)) {
        return def.default_value;
    }
    PropertyValue out = def.default_value;
    if (prefs.isKey(def.key)) {
        switch (def.type) {
            case PropertyType::Bool:
                out = PropertyValue::from_bool(prefs.getBool(def.key, def.default_value.as_bool()));
                break;
            case PropertyType::U8:
            case PropertyType::Enum: {
                uint8_t v = prefs.getUChar(def.key, def.default_value.u8);
                if (def.type == PropertyType::U8)
                    out = PropertyValue::from_u8(v);
                else
                    out = PropertyValue::from_enum(v);
                break;
            }
            case PropertyType::U16:
                out = PropertyValue::from_u16(prefs.getUShort(def.key, def.default_value.as_u16()));
                break;
            case PropertyType::Colour:
                out = PropertyValue::from_colour(prefs.getUInt(def.key, def.default_value.as_colour()));
                break;
        }
    }
    prefs.end();
    return clamp_to_bounds(def, out);
}

bool write_to_nvs(const char* ns_name, const PropertyDef& def, PropertyValue value) {
    Preferences prefs;
    if (!prefs.begin(ns_name, /*readOnly=*/false)) return false;
    bool ok = false;
    switch (def.type) {
        case PropertyType::Bool:
            ok = prefs.putBool(def.key, value.as_bool()) > 0
              || prefs.getBool(def.key) == value.as_bool();
            break;
        case PropertyType::U8:
            ok = prefs.putUChar(def.key, value.as_u8()) > 0;
            break;
        case PropertyType::U16:
            ok = prefs.putUShort(def.key, value.as_u16()) > 0;
            break;
        case PropertyType::Colour:
            ok = prefs.putUInt(def.key, value.as_colour()) > 0;
            break;
        case PropertyType::Enum:
            ok = prefs.putUChar(def.key, value.as_enum()) > 0;
            break;
    }
    prefs.end();
    return ok;
}

}  // namespace

PropertyBag::PropertyBag(const Plugin& plugin) : plugin_(plugin) {
    auto* impl = new ArduinoImpl();
    compose_namespace(plugin_, impl->ns_name, sizeof(impl->ns_name));
    impl_ = impl;
}

PropertyBag::~PropertyBag() {
    delete static_cast<ArduinoImpl*>(impl_);
    impl_ = nullptr;
}

PropertyValue PropertyBag::get(const char* key) const {
    const PropertyDef* def = find_def(plugin_, key);
    if (!def) return PropertyValue{};

    auto* impl = static_cast<ArduinoImpl*>(impl_);
    int slot = impl->find_cache_slot(key);
    if (slot >= 0) {
        return clamp_to_bounds(*def, impl->cache[slot].value);
    }
    PropertyValue value = read_from_nvs(impl->ns_name, *def);
    impl->cache_put(key, value);
    return value;
}

bool PropertyBag::set(const char* key, PropertyValue value) {
    const PropertyDef* def = find_def(plugin_, key);
    if (!def) return false;
    if (def->type != value.type) return false;

    PropertyValue clamped = clamp_to_bounds(*def, value);
    auto* impl = static_cast<ArduinoImpl*>(impl_);
    bool ok = write_to_nvs(impl->ns_name, *def, clamped);
    if (ok) {
        impl->cache_put(key, clamped);
    }
    return ok;
}

void PropertyBag::clear_for_tests() {
    // No-op on Arduino - NVS persistence is the entire point. Tests run
    // on the native build only.
}

#else  // !ARDUINO

// -----------------------------------------------------------------------------
// Native implementation: in-memory map keyed by namespace+key
// -----------------------------------------------------------------------------

namespace {

struct NativeEntry {
    std::string   ns;
    std::string   key;
    PropertyValue value;
};

// Static store. Shared across all bags so that two bags addressing the
// same plugin id (rare in tests) see each other's writes.
struct NativeStore {
    std::vector<NativeEntry> entries;

    NativeEntry* find(const std::string& ns, const std::string& key) {
        for (auto& e : entries) {
            if (e.ns == ns && e.key == key) return &e;
        }
        return nullptr;
    }

    void put(const std::string& ns, const std::string& key, PropertyValue value) {
        if (auto* e = find(ns, key)) {
            e->value = value;
            return;
        }
        entries.push_back({ns, key, value});
    }

    void clear() { entries.clear(); }
};

NativeStore& native_store() {
    static NativeStore s;
    return s;
}

struct NativeImpl {
    std::string ns;
};

}  // namespace

PropertyBag::PropertyBag(const Plugin& plugin) : plugin_(plugin) {
    auto* impl = new NativeImpl();
    char buf[16];
    compose_namespace(plugin_, buf, sizeof(buf));
    impl->ns = buf;
    impl_ = impl;
}

PropertyBag::~PropertyBag() {
    delete static_cast<NativeImpl*>(impl_);
    impl_ = nullptr;
}

PropertyValue PropertyBag::get(const char* key) const {
    const PropertyDef* def = find_def(plugin_, key);
    if (!def) return PropertyValue{};

    auto* impl = static_cast<NativeImpl*>(impl_);
    auto* entry = native_store().find(impl->ns, key);
    PropertyValue value = entry ? entry->value : def->default_value;
    return clamp_to_bounds(*def, value);
}

bool PropertyBag::set(const char* key, PropertyValue value) {
    const PropertyDef* def = find_def(plugin_, key);
    if (!def) return false;
    if (def->type != value.type) return false;

    PropertyValue clamped = clamp_to_bounds(*def, value);
    auto* impl = static_cast<NativeImpl*>(impl_);
    native_store().put(impl->ns, key, clamped);
    return true;
}

void PropertyBag::clear_for_tests() {
    native_store().clear();
}

#endif  // ARDUINO

}  // namespace plugins
}  // namespace nocturnation
