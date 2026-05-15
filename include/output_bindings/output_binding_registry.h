// OutputBindingRegistry - global registry of registered output bindings.
//
// Aliases plugins::Registry<OutputBinding, 16>. The capacity (16) is a
// soft upper bound on how many bindings a single build links; the
// codebase will ship two initially (LocalDisplayBinding +
// PixMobIrBinding from Block 9) but extra room costs nothing for
// future DMX-out, Tildagon LED ring, etc.
//
// main.cpp / setup() calls
// output_binding_registry().register_plugin(...) for each binding
// instance at boot. The Lume mode walks the registry on entry to
// activate the per-Lume-config subset (config maps existing
// `slv_ir_grp` etc. NVS keys onto per-binding properties in Block 9).

#pragma once

#include "plugins/registry.h"
#include "output_bindings/output_binding.h"

namespace nocturnation {
namespace output_bindings {

using OutputBindingRegistry = plugins::Registry<OutputBinding, 16>;

OutputBindingRegistry& output_binding_registry();

}  // namespace output_bindings
}  // namespace nocturnation
