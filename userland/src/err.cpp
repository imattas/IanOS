#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[err] stdout online");
    hybrid::user::write_error("[err] stderr online\n");
    hybrid::user::exit(7);
}
