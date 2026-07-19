#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[whoami] root");
    hybrid::user::exit(0);
}
