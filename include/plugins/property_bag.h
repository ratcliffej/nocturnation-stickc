// PropertyBag - per-plugin storage for property values.
//
// On Arduino: backed by ESP-IDF Preferences (NVS). Namespace is derived
// from the plugin's id() and kind() as "nv_<id>" (Visualisation) or
// "nb_<id>" (OutputBinding). NVS namespaces have a 15-character limit
// so plugin ids are constrained to <= 12 characters. A small in-RAM
// cache (kCacheSize entries) avoids hammering NVS on repeated get()
// calls inside a single frame; plugins declaring more than that many
// properties still work but reads beyond the cache miss into NVS more
// often.
//
// On native test builds (#ifndef ARDUINO): in-memory only. No NVS, no
// persistence. clear_for_tests() empties the bag - the test seam Block 3's
// unit tests use to reset state between cases.
//
// One bag per plugin instance. Construct it against the plugin and the
// schema is read from plugin.properties() lazily on first access.

#pragma once

#include "plugins/plugin.h"

namespace nocturnation {
namespace plugins {

class PropertyBag {
public:
    explicit PropertyBag(const Plugin& plugin);
    ~PropertyBag();

    // Returns the current value for `key`. Falls back to the property's
    // default if never set (or NVS read fails). Numeric/Enum values are
    // clamped to [min, max] from the schema. Returns a default-constructed
    // PropertyValue if `key` isn't in the plugin's schema.
    PropertyValue get(const char* key) const;

    // Sets and persists `value` for `key`. Type-checks against the schema
    // (rejected if the value's type doesn't match the schema's type).
    // Numeric/Enum values are clamped to [min, max] before being stored.
    // Returns true on success.
    bool set(const char* key, PropertyValue value);

    // Test seam: clears in-memory state on native builds. No-op on Arduino
    // (NVS persistence is the entire point there).
    static void clear_for_tests();

private:
    const Plugin& plugin_;
    void*         impl_ = nullptr;  // pimpl - opaque per-build implementation
};

}  // namespace plugins
}  // namespace nocturnation
