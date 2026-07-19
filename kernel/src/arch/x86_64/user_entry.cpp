#include "hk/arch/x86_64/user_entry.hpp"

extern "C" [[noreturn]] void enter_user_mode(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t rflags, uint64_t cs, uint64_t ss);

namespace hk::arch::x86_64 {

[[noreturn]] void enter_user(uint64_t rip, uint64_t rsp, uint64_t cr3, uint64_t rflags, uint16_t cs, uint16_t ss) {
    enter_user_mode(rip, rsp, cr3, rflags, cs, ss);
}

}
