#include "shows/show_registry.h"

namespace nocturnation {
namespace shows {

ShowRegistry& show_registry() {
    static ShowRegistry s_instance;
    return s_instance;
}

}  // namespace shows
}  // namespace nocturnation
