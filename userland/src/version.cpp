#include "hybrid/user.hpp"

namespace {

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

bool starts_with(const char* text, const char* prefix) {
    uint64_t i = 0;
    while (prefix[i] != 0) {
        if (text[i] != prefix[i]) return false;
        ++i;
    }
    return true;
}

void trim_newline(char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] == '\n' || text[i] == '\r') {
            text[i] = 0;
            return;
        }
    }
}

bool read_path(const char* path, char* buffer, uint64_t capacity) {
    if (capacity == 0) return false;
    buffer[0] = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone) return false;
    auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, opened.value, reinterpret_cast<uint64_t>(buffer), capacity - 1);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    if (read.error != hybrid::kSyscallErrorNone) return false;
    buffer[read.value < capacity ? read.value : capacity - 1] = 0;
    trim_newline(buffer);
    return true;
}

void write_pair(const char* label, const char* value) {
    hybrid::user::write_text_line("[version] ", label, value);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::SystemInfo system{};
    auto system_result = hybrid::user::syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&system));
    if (system_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[version] ", "system error ", system_result.error);
        hybrid::user::exit(1);
    }

    char osrelease[64];
    char kernel_version[96];
    bool read_release = read_path("/proc/sys/kernel/osrelease", osrelease, sizeof(osrelease));
    bool read_kernel = read_path("/proc/sys/kernel/version", kernel_version, sizeof(kernel_version));

    write_pair("os ", system.sysname);
    write_pair("release ", system.release);
    write_pair("machine ", system.machine);
    write_pair("kernel ", system.kernel_type);
    write_pair("boot ", system.boot_mode);
    if (read_release) write_pair("proc release ", osrelease);
    if (read_kernel) write_pair("proc kernel ", kernel_version);
    hybrid::user::write_hex_line("[version] ", "bootinfo ", system.boot_info_version);
    hybrid::user::write_hex_line("[version] ", "modules ", system.boot_module_count);

    bool consistent = read_release && read_kernel &&
        streq(osrelease, system.release) &&
        starts_with(kernel_version, system.kernel_type);
    hybrid::user::write_text_line("[version] ", "status ", consistent ? "ok" : "mismatch");
    hybrid::user::exit(consistent ? 0 : 2);
}
