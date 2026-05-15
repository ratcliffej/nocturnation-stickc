// ShowRegistry - global registry of registered Shows (Epic 4.7).
//
// Aliases plugins::Registry<Show, 16>. Capacity (16) is a soft upper
// bound on how many Shows a single build links; Block 1 ships one
// (SimpleBeatShow), Block 5 adds DynamicShow. Extra capacity costs
// nothing.
//
// main.cpp / setup() calls show_registry().register_plugin(...) for
// each Show instance at boot. DirectorMode walks the registry
// to populate the show picker and resolve the operator's last-used
// show on entry (via persistence::load_active_show_id).

#pragma once

#include "plugins/registry.h"
#include "shows/show.h"

namespace nocturnation {
namespace shows {

using ShowRegistry = plugins::Registry<Show, 16>;

ShowRegistry& show_registry();

}  // namespace shows
}  // namespace nocturnation
