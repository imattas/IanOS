#include "hybrid/user.hpp"

namespace {

void write_field(const char* label, const char* value) {
    hybrid::user::write_text_line("[sysinfo] ", label, value);
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::SystemInfo info;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[sysinfo] ", "error ", result.error);
        hybrid::user::exit(1);
    }
    write_field("sysname ", info.sysname);
    write_field("release ", info.release);
    write_field("machine ", info.machine);
    write_field("boot ", info.boot_mode);
    write_field("kernel ", info.kernel_type);
    hybrid::user::write_hex_line("[sysinfo] ", "modules ", info.boot_module_count);
    hybrid::user::write_hex_line("[sysinfo] ", "kernel base ", info.kernel_base);
    hybrid::user::write_hex_line("[sysinfo] ", "kernel end ", info.kernel_end);
    hybrid::user::write_hex_line("[sysinfo] ", "rsdp ", info.rsdp);
    hybrid::user::exit(0);
}
