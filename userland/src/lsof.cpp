#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint64_t parse_number(const char* text, bool& ok) {
    ok = false;
    if (!text || text[0] == 0) return 0;
    uint64_t value = 0;
    uint64_t i = 0;
    bool hex = text[0] == '0' && (text[1] == 'x' || text[1] == 'X');
    if (hex) i = 2;
    if (text[i] == 0) return 0;
    for (; text[i] != 0; ++i) {
        uint8_t digit = 0;
        if (text[i] >= '0' && text[i] <= '9') {
            digit = static_cast<uint8_t>(text[i] - '0');
        } else if (hex && text[i] >= 'a' && text[i] <= 'f') {
            digit = static_cast<uint8_t>(10 + text[i] - 'a');
        } else if (hex && text[i] >= 'A' && text[i] <= 'F') {
            digit = static_cast<uint8_t>(10 + text[i] - 'A');
        } else {
            return 0;
        }
        value = hex ? ((value << 4) | digit) : (value * 10 + digit);
    }
    ok = true;
    return value;
}

const char* kind_name(hybrid::FileDescriptorInfoKind kind) {
    switch (kind) {
    case hybrid::FileDescriptorInfoKind::Vfs: return "vfs";
    case hybrid::FileDescriptorInfoKind::PipeRead: return "pipe-read";
    case hybrid::FileDescriptorInfoKind::PipeWrite: return "pipe-write";
    default: return "empty";
    }
}

void clear_process(hybrid::ProcessInfo& process) {
    auto* bytes = reinterpret_cast<unsigned char*>(&process);
    for (uint64_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

void clear_fd(hybrid::FileDescriptorInfo& info) {
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
}

void write_fd_line(const hybrid::ProcessInfo& process, const hybrid::FileDescriptorInfo& info) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lsof] pid ");
    hybrid::user::append_hex(line, sizeof(line), cursor, process.pid);
    hybrid::user::append_text(line, sizeof(line), cursor, " fd ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.fd);
    hybrid::user::append_text(line, sizeof(line), cursor, " kind ");
    hybrid::user::append_text(line, sizeof(line), cursor, kind_name(info.kind));
    if (info.kind == hybrid::FileDescriptorInfoKind::Vfs) {
        hybrid::user::append_text(line, sizeof(line), cursor, " offset ");
        hybrid::user::append_hex(line, sizeof(line), cursor, info.offset);
        hybrid::user::append_text(line, sizeof(line), cursor, " path ");
        hybrid::user::append_text(line, sizeof(line), cursor, info.path);
    } else if (info.kind == hybrid::FileDescriptorInfoKind::PipeRead || info.kind == hybrid::FileDescriptorInfoKind::PipeWrite) {
        hybrid::user::append_text(line, sizeof(line), cursor, " pipe ");
        hybrid::user::append_hex(line, sizeof(line), cursor, info.pipe_id);
    }
    hybrid::user::append_text(line, sizeof(line), cursor, " cmd ");
    hybrid::user::append_text(line, sizeof(line), cursor, process.name);
    hybrid::user::write_line(line);
}

uint64_t inspect_process(const hybrid::ProcessInfo& process) {
    uint64_t count = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        hybrid::FileDescriptorInfo info;
        clear_fd(info);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetFileDescriptorInfo, process.pid, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        write_fd_line(process, info);
        ++count;
    }
    return count;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    uint64_t filter_pid = 0;
    hybrid::ArgumentInfo arg;
    if (get_arg(1, arg)) {
        bool ok = false;
        filter_pid = parse_number(arg.value, ok);
        if (!ok || filter_pid == 0) {
            hybrid::user::write_line("[lsof] usage lsof [pid]");
            hybrid::user::exit(1);
        }
    }

    auto process_count = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (process_count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lsof] ", "process count error ", process_count.error);
        hybrid::user::exit(2);
    }

    uint64_t fd_count = 0;
    for (uint64_t i = 0; i < process_count.value; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (process.pid == 0 || process.state == 4) continue;
        if (filter_pid != 0 && process.pid != filter_pid) continue;
        fd_count += inspect_process(process);
    }

    hybrid::user::write_hex_line("[lsof] ", "fds ", fd_count);
    hybrid::user::exit(fd_count == 0 ? 1 : 0);
}
