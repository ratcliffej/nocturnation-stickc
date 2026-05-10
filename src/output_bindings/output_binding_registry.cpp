#include "output_bindings/output_binding_registry.h"

namespace nocturnation {
namespace output_bindings {

OutputBindingRegistry& output_binding_registry() {
    static OutputBindingRegistry s_instance;
    return s_instance;
}

}  // namespace output_bindings
}  // namespace nocturnation
