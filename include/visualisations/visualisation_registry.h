// VisualisationRegistry - global registry of registered visualisations.
//
// Aliases plugins::Registry<Visualisation, 16>. The capacity (16) is a
// soft upper bound on how many vis a single build links; the codebase
// only ships two (BeatPulse from Block 8, SpectrumBars from Block 11)
// but extra room costs nothing.
//
// main.cpp / setup() calls visualisation_registry().register_plugin(...)
// for each vis instance at boot. The active mode (Block 8's
// autonomous_master) walks the registry to populate the vis picker and
// resolve the user's last-used vis on entry.

#pragma once

#include "plugins/registry.h"
#include "visualisations/visualisation.h"

namespace nocturnation {
namespace visualisations {

using VisualisationRegistry = plugins::Registry<Visualisation, 16>;

VisualisationRegistry& visualisation_registry();

}  // namespace visualisations
}  // namespace nocturnation
