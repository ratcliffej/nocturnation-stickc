// CapabilityMask - compact bitset wrapping hal::Capability values.
//
// Used by the plugin base class (`Plugin::required_capabilities()`) to
// declare what a visualisation or output binding needs from the host.
// The framework matches a plugin's mask against the active backend's
// declared capabilities (via `HAL::has(...)` over each set bit) and
// gates the plugin off if any requirement is missing.
//
// The Capability enum currently has 13 values (max value
// `AnalyserSectionDetection` = 12). uint32_t is comfortable headroom
// for the four reserved slots already in the enum and any near-future
// additions before we'd need to widen this.

#pragma once

#include <cstdint>

#include "hal/hal.h"

namespace nocturnation {
namespace hal {

class CapabilityMask {
public:
    constexpr CapabilityMask() = default;

    constexpr CapabilityMask& set(Capability c) {
        bits_ |= (uint32_t{1} << static_cast<uint8_t>(c));
        return *this;
    }

    constexpr bool has(Capability c) const {
        return ((bits_ >> static_cast<uint8_t>(c)) & 1u) != 0u;
    }

    // True when every capability in *this is also in `other`. Used for
    // requirement matching: `plugin.required_capabilities().subset_of(host_mask)`.
    constexpr bool subset_of(CapabilityMask other) const {
        return (bits_ & other.bits_) == bits_;
    }

    constexpr uint32_t raw() const { return bits_; }
    constexpr bool     empty() const { return bits_ == 0; }

private:
    uint32_t bits_ = 0;
};

// Helper: build a CapabilityMask from a parameter pack of Capability values.
//   constexpr auto m = make_capability_mask(Capability::Mic, Capability::Display);
template <typename... Caps>
constexpr CapabilityMask make_capability_mask(Caps... caps) {
    CapabilityMask m;
    (m.set(caps), ...);
    return m;
}

}  // namespace hal
}  // namespace nocturnation
