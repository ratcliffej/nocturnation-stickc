// Registry<PluginT> - templated, fixed-capacity, explicitly-registered
// catalogue of plugin singletons.
//
// One registry per plugin kind: VisualisationRegistry (Block 5),
// OutputBindingRegistry (Block 6) - both alias Registry<T> with a
// suitable Capacity.
//
// No static-init magic: plugins are added at startup (or in tests) via
// register_plugin(). Capacity is bounded at compile time so the storage
// is a plain array; no heap, no surprises on Arduino.

#pragma once

#include <cstddef>

#include "plugins/plugin.h"

namespace nocturnation {
namespace plugins {

namespace detail {
// Header-local strcmp avoiding <cstring> in plugin headers.
constexpr bool registry_id_equal(const char* a, const char* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    while (*a && *b) {
        if (*a != *b) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}
}  // namespace detail

template <typename PluginT, size_t Capacity = 16>
class Registry {
public:
    bool register_plugin(PluginT* p) {
        if (!p) return false;
        if (count_ >= Capacity) return false;
        for (size_t i = 0; i < count_; ++i) {
            if (entries_[i] == p) return false;  // already registered
        }
        entries_[count_++] = p;
        return true;
    }

    PluginT* find(const char* id) const {
        if (!id) return nullptr;
        for (size_t i = 0; i < count_; ++i) {
            if (detail::registry_id_equal(entries_[i]->id(), id)) return entries_[i];
        }
        return nullptr;
    }

    PluginT* at(size_t index) const {
        return (index < count_) ? entries_[index] : nullptr;
    }

    size_t count() const { return count_; }
    constexpr size_t capacity() const { return Capacity; }

    // Test seam.
    void clear() { count_ = 0; }

private:
    PluginT* entries_[Capacity] = {};
    size_t   count_             = 0;
};

}  // namespace plugins
}  // namespace nocturnation
