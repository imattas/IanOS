#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::user::write_line("[ubusy] child start");
    volatile uint64_t value = 0;
    for (uint64_t i = 0; i < 5000000ull; ++i) {
        value += i ^ (value << 1);
        asm volatile("pause" : : : "memory");
    }
    if (value == 0xffffffffffffffffull) hybrid::user::write_line("[ubusy] impossible");
    hybrid::user::write_line("[ubusy] child done");
    hybrid::user::exit(33);
}
