#include "visualisations/visualisation_registry.h"

namespace nocturnation {
namespace visualisations {

VisualisationRegistry& visualisation_registry() {
    static VisualisationRegistry s_instance;
    return s_instance;
}

}  // namespace visualisations
}  // namespace nocturnation
