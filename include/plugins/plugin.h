// Plugin - shared base for the two plugin kinds in Epic 4.6:
//   - Visualisation (Block 5)         - master-side rendering of audio events
//   - OutputBinding (Block 6)         - slave-side fan-out of render events
//
// This block (Block 3) lands only the contract and supporting types.
// Concrete plugins arrive in Blocks 5+. Headers stay heap-free; the
// implementation .cpp is free to use std::vector / std::string for
// native test builds but Arduino paths use fixed-size storage only.

#pragma once

#include <cstddef>
#include <cstdint>

#include "hal/capability_mask.h"

namespace nocturnation {
namespace plugins {

// =============================================================================
// PropertyType / PropertyValue / PropertyDef
// =============================================================================

enum class PropertyType : uint8_t {
    Bool,
    U8,      // 0..255
    U16,     // 0..65535
    Colour,  // packed 0x00RRGGBB
    Enum,    // U8 with named values for UI; min/max bound the index range
};

// Tagged union of the five property representations. Trivially copyable
// so it can be passed by value through the property bag API and stored
// in a small fixed-size cache.
struct PropertyValue {
    PropertyType type;
    union {
        bool     b;
        uint8_t  u8;
        uint16_t u16;
        uint32_t colour;
        uint8_t  enum_index;
        uint32_t raw;  // for default-init / raw access
    };

    constexpr PropertyValue() : type(PropertyType::Bool), raw(0) {}

    static constexpr PropertyValue from_bool(bool v) {
        PropertyValue p;
        p.type = PropertyType::Bool;
        p.b = v;
        return p;
    }
    static constexpr PropertyValue from_u8(uint8_t v) {
        PropertyValue p;
        p.type = PropertyType::U8;
        p.u8 = v;
        return p;
    }
    static constexpr PropertyValue from_u16(uint16_t v) {
        PropertyValue p;
        p.type = PropertyType::U16;
        p.u16 = v;
        return p;
    }
    static constexpr PropertyValue from_colour(uint32_t v) {
        PropertyValue p;
        p.type = PropertyType::Colour;
        p.colour = v;
        return p;
    }
    static constexpr PropertyValue from_enum(uint8_t v) {
        PropertyValue p;
        p.type = PropertyType::Enum;
        p.enum_index = v;
        return p;
    }

    constexpr bool     as_bool()   const { return b; }
    constexpr uint8_t  as_u8()     const { return u8; }
    constexpr uint16_t as_u16()    const { return u16; }
    constexpr uint32_t as_colour() const { return colour; }
    constexpr uint8_t  as_enum()   const { return enum_index; }
};

// Property schema entry. Plugins declare a static array of these and
// return a Span over it from `properties()`. The framework uses the
// schema for: NVS persistence, UI auto-generation, bounds clamping,
// and type validation on writes.
struct PropertyDef {
    const char*   key;            // <= 15 chars (NVS key constraint); kebab-case or snake_case
    PropertyType  type;
    PropertyValue default_value;
    PropertyValue min_value;      // for U8/U16/Enum: lower bound (inclusive). Bool/Colour: ignored.
    PropertyValue max_value;      // for U8/U16/Enum: upper bound (inclusive). Bool/Colour: ignored.
    const char*   display_name;   // human-readable for UI
    const char*   unit;           // optional; nullptr if none. e.g. "ms", "%", "°", "Hz".
    // For Enum type only: array of enum value names. Length = (max.as_enum() - min.as_enum() + 1).
    const char* const* enum_names;
};

// =============================================================================
// PowerProfile - what a plugin needs from the audio + display pipeline
// =============================================================================
//
// Block 7 ("Pipeline gating") uses these flags to decide which optional
// per-frame surfaces the framework subscribes to on the plugin's behalf.
// Plugins declare their needs here.
struct PowerProfile {
    bool     needs_audio_frames    = true;   // most vis/bindings need beats
    bool     needs_spectrum_frame  = false;  // when true, framework subscribes to
                                             //  SpectrumFrameEvent. With no
                                             //  subscriber the per-frame fan-out
                                             //  copy + dispatch is skipped in
                                             //  LocalDriver. The underlying FFT
                                             //  roll-up still runs because
                                             //  BeatDetector consumes it
                                             //  in-pipeline.
    bool     needs_8band_summary   = false;  // perceptual bands: opt-in
    uint16_t lcd_refresh_hz_max    = 20;     // most vis. Static 1.
    uint16_t tick_hz               = 0;      // 0 = audio-driven only. >0 = framework calls tick() at this rate.
};

// =============================================================================
// Span<T> - tiny stand-in for std::span (not available in C++14/17 envs)
// =============================================================================
//
// Used as the return type of Plugin::properties(). Just enough surface
// for "view a contiguous const array"; nothing more.
template <typename T>
struct Span {
    const T* data = nullptr;
    size_t   size = 0;

    constexpr Span() = default;
    constexpr Span(const T* d, size_t n) : data(d), size(n) {}

    template <size_t N>
    constexpr Span(const T (&arr)[N]) : data(arr), size(N) {}

    constexpr const T* begin() const { return data; }
    constexpr const T* end()   const { return data + size; }
    constexpr const T& operator[](size_t i) const { return data[i]; }
};

// =============================================================================
// Plugin base class
// =============================================================================

// Plugin kind - used for NVS namespace prefix and registry separation.
//   Visualisation -> NVS namespace "nv_<id>"  (Epic 4.6 - widget candidates post-4.7)
//   OutputBinding -> NVS namespace "nb_<id>"
//   Show          -> NVS namespace "ns_<id>"  (Epic 4.7 - master-side performance)
enum class PluginKind : uint8_t {
    Visualisation = 0,
    OutputBinding = 1,
    Show          = 2,
};

// Stable, polymorphic interface. Concrete subclasses (Visualisation in
// Block 5, OutputBinding in Block 6) extend this and add their own
// virtual lifecycle / event hooks on top.
//
// Convention: id() must be <= 12 characters so the NVS namespace
// "nv_<id>" or "nb_<id>" fits within the 15-character ESP-IDF
// Preferences namespace limit. Not enforced at compile time (would
// require constexpr id() which conflicts with the virtual signature);
// later we can add a debug-build assertion.
class Plugin {
public:
    virtual ~Plugin() = default;

    // Stable string identity. <= 12 chars by convention (see above).
    virtual const char* id() const = 0;

    // Human-readable name for menus / settings screens.
    virtual const char* display_name() const = 0;

    // What this plugin needs from the host. Empty mask = no requirements.
    virtual hal::CapabilityMask required_capabilities() const { return {}; }

    // Property schema. Empty span = no properties.
    virtual Span<const PropertyDef> properties() const { return {}; }

    // Power needs. Default profile is "ordinary audio-driven vis".
    virtual PowerProfile power() const { return {}; }

    // Which plugin-kind namespace this lives in (for NVS + registry).
    virtual PluginKind kind() const = 0;
};

}  // namespace plugins
}  // namespace nocturnation
