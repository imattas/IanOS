#include "hybrid/user.hpp"

namespace {

constexpr const char* kKeys[] = {
    "kernel.hostname",
    "kernel.ostype",
    "kernel.osrelease",
    "kernel.pid_max",
    "kernel.threads-max",
    "kernel.boot_mode",
    "kernel.boot_flags",
    "kernel.version",
};

constexpr const char* kPaths[] = {
    "/proc/sys/kernel/hostname",
    "/proc/sys/kernel/ostype",
    "/proc/sys/kernel/osrelease",
    "/proc/sys/kernel/pid_max",
    "/proc/sys/kernel/threads-max",
    "/proc/sys/kernel/boot_mode",
    "/proc/sys/kernel/boot_flags",
    "/proc/sys/kernel/version",
};

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

bool get_argument(uint64_t index, hybrid::ArgumentInfo& out) {
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

void trim_newline(char* text) {
    uint64_t i = 0;
    while (text[i] != 0) {
        if (text[i] == '\n' || text[i] == '\r') {
            text[i] = 0;
            return;
        }
        ++i;
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

void print_pair(const char* key, const char* value) {
    char line[160];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[sysctl] ");
    hybrid::user::append_text(line, sizeof(line), cursor, key);
    hybrid::user::append_text(line, sizeof(line), cursor, " = ");
    hybrid::user::append_text(line, sizeof(line), cursor, value);
    hybrid::user::write_line(line);
}

int key_index(const char* key) {
    for (uint64_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
        if (streq(key, kKeys[i])) return static_cast<int>(i);
    }
    return -1;
}

void print_usage() {
    hybrid::user::write_line("[sysctl] usage sysctl [-a] [kernel.hostname|kernel.ostype|kernel.osrelease|kernel.pid_max|kernel.threads-max|kernel.boot_mode|kernel.boot_flags|kernel.version]");
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto argc = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argc.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[sysctl] ", "argc error ", argc.error);
        hybrid::user::exit(1);
    }

    uint64_t start = 1;
    bool list_all = argc.value <= 1;
    if (argc.value > 1) {
        hybrid::ArgumentInfo first{};
        if (!get_argument(1, first)) {
            hybrid::user::write_line("[sysctl] argument error");
            hybrid::user::exit(2);
        }
        if (streq(first.value, "-a")) {
            list_all = true;
            start = 2;
        }
    }

    char value[96];
    bool ok = true;
    if (list_all && start >= argc.value) {
        for (uint64_t i = 0; i < sizeof(kKeys) / sizeof(kKeys[0]); ++i) {
            if (!read_path(kPaths[i], value, sizeof(value))) {
                hybrid::user::write_text_line("[sysctl] ", "read error ", kKeys[i]);
                ok = false;
                continue;
            }
            print_pair(kKeys[i], value);
        }
        hybrid::user::exit(ok ? 0 : 3);
    }

    for (uint64_t i = start; i < argc.value; ++i) {
        hybrid::ArgumentInfo arg{};
        if (!get_argument(i, arg)) {
            hybrid::user::write_line("[sysctl] argument error");
            hybrid::user::exit(2);
        }
        int index = key_index(arg.value);
        if (index < 0) {
            print_usage();
            hybrid::user::write_text_line("[sysctl] ", "unknown key ", arg.value);
            hybrid::user::exit(4);
        }
        if (!read_path(kPaths[index], value, sizeof(value))) {
            hybrid::user::write_text_line("[sysctl] ", "read error ", arg.value);
            hybrid::user::exit(3);
        }
        print_pair(arg.value, value);
    }

    hybrid::user::exit(0);
}
