#include "hybrid/user.hpp"

namespace {

char g_chunk[128];

void clear_argument(hybrid::ArgumentInfo& out) {
    for (uint64_t i = 0; i < sizeof(out.value); ++i) out.value[i] = 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    clear_argument(out);
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool parse_id(const char* text, uint64_t& value) {
    value = 0;
    if (!text || text[0] == 0) return false;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return value != 0;
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[24];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

void build_task_path(char (&path)[64], uint64_t pid) {
    uint64_t cursor = 0;
    hybrid::user::append_text(path, sizeof(path), cursor, "/proc/");
    append_decimal(path, sizeof(path), cursor, pid);
    hybrid::user::append_text(path, sizeof(path), cursor, "/task");
}

void build_task_status_path(char (&path)[64], uint64_t pid, uint64_t tid) {
    uint64_t cursor = 0;
    hybrid::user::append_text(path, sizeof(path), cursor, "/proc/");
    append_decimal(path, sizeof(path), cursor, pid);
    hybrid::user::append_text(path, sizeof(path), cursor, "/task/");
    append_decimal(path, sizeof(path), cursor, tid);
    hybrid::user::append_text(path, sizeof(path), cursor, "/status");
}

bool read_directory_entry(const char* path, uint64_t index, hybrid::VfsDirectoryEntryInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::ReadDirectory,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        index,
                                        reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

bool list_tasks(const char* path) {
    hybrid::user::write_text_line("[proctask] ", "path ", path);
    uint64_t entries = 0;
    for (;;) {
        hybrid::VfsDirectoryEntryInfo entry{};
        if (!read_directory_entry(path, entries, entry)) break;
        char line[128];
        uint64_t cursor = 0;
        hybrid::user::append_text(line, sizeof(line), cursor, "[proctask] tid ");
        hybrid::user::append_text(line, sizeof(line), cursor, entry.name);
        hybrid::user::append_text(line, sizeof(line), cursor, " path ");
        hybrid::user::append_text(line, sizeof(line), cursor, entry.path);
        hybrid::user::write_line(line);
        ++entries;
    }
    hybrid::user::write_hex_line("[proctask] ", "entries ", entries);
    return entries != 0;
}

bool stream_status(const char* path, uint64_t& bytes) {
    bytes = 0;
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[proctask] ", "open error ", opened.error);
        return false;
    }
    hybrid::user::write_text_line("[proctask] ", "path ", path);
    hybrid::user::write_text("[proctask] ");
    for (;;) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle,
                                          opened.value,
                                          reinterpret_cast<uint64_t>(g_chunk),
                                          sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && bytes != 0) break;
            hybrid::user::write_hex_line("[proctask] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        bytes += read.value;
        hybrid::user::syscall(hybrid::SyscallNumber::Write,
                              hybrid::kStdoutFd,
                              reinterpret_cast<uint64_t>(g_chunk),
                              read.value);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return bytes != 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto argc = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argc.error != hybrid::kSyscallErrorNone) hybrid::user::exit(1);

    if (argc.value <= 1) {
        static const char self_task[] = "/proc/self/task";
        hybrid::user::exit(list_tasks(self_task) ? 0 : 1);
    }

    hybrid::ArgumentInfo pid_arg{};
    if (!get_arg(1, pid_arg)) hybrid::user::exit(1);
    uint64_t pid = 0;
    if (!parse_id(pid_arg.value, pid)) {
        hybrid::user::write_line("[proctask] usage proctask [pid [tid]]");
        hybrid::user::exit(2);
    }

    if (argc.value == 2) {
        char path[64]{};
        build_task_path(path, pid);
        hybrid::user::exit(list_tasks(path) ? 0 : 1);
    }

    hybrid::ArgumentInfo tid_arg{};
    if (!get_arg(2, tid_arg)) hybrid::user::exit(1);
    uint64_t tid = 0;
    if (!parse_id(tid_arg.value, tid)) {
        hybrid::user::write_line("[proctask] usage proctask [pid [tid]]");
        hybrid::user::exit(2);
    }
    char path[64]{};
    build_task_status_path(path, pid, tid);
    uint64_t bytes = 0;
    if (!stream_status(path, bytes)) hybrid::user::exit(1);
    hybrid::user::write_hex_line("[proctask] ", "bytes ", bytes);
    hybrid::user::exit(0);
}
