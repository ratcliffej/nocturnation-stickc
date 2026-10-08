// Shared property-bag apply logic (Epic 20 B3c / Epic 21 B3d).
//
// A single callback that recognises every well-known key in the
// NocturNation property bag ([[wire-v0x04-shipped]] + Epic 20 §5)
// and persists the valid ones to NVS. Used by both:
//
//   - BLE config characteristic writes (ble_service.cpp onWrite path),
//   - ESP-NOW CONFIG_WRITE frames (config_rx.cpp dispatcher).
//
// Factored out of ble_service.cpp at Epic 21 B3d so the two write
// paths share a single source of truth for key -> persistence
// routing. Behaviour is bit-identical to the pre-B3d BLE path -
// moving the function body across TUs doesn't change what any
// recognised key does.
//
// Native-safe: calls into modes::persistence::save_*, which have
// native stubs that store into process-static variables.

#pragma once

#include "ble/property_bag_tlv.h"
#include "ble/ble_service.h"   // for Role (same enum values on both paths)

namespace nocturnation {
namespace ble {

// Context the callback mutates while walking a bag. `role` selects
// which key subset applies; `value_out_of_range` is set true if any
// recognised key had a bad value (the walk continues - partial apply
// is the documented contract).
struct ApplyCtx {
    Role role;
    bool value_out_of_range;
};

// TLV decoder callback. Returns true to continue iterating the bag.
// Unrecognised keys are silently accepted (forward-compat, Epic 20 §4).
bool apply_config_entry(const TlvEntry& e, void* raw_ctx);

}  // namespace ble
}  // namespace nocturnation
