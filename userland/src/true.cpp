#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[true] ok");
    hybrid::user::exit(0);
}
