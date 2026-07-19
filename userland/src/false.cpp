#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[false] fail");
    hybrid::user::exit(1);
}
