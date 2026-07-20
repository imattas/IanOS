#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint64_t parse_decimal(const char* text) {
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return 0;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
    }
    return value;
}

void clear_process(hybrid::ProcessInfo& process) {
    auto* bytes = reinterpret_cast<unsigned char*>(&process);
    for (uint64_t i = 0; i < sizeof(process); ++i) bytes[i] = 0;
}

bool copy_process_by_pid(uint64_t pid, hybrid::ProcessInfo& out) {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ProcessInfo process;
        clear_process(process);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        if (process.pid == pid) {
            out = process;
            return true;
        }
    }
    return false;
}

void write_mapping(const char* name, uint64_t start, uint64_t pages) {
    char line[192];
    uint64_t cursor = 0;
    const uint64_t bytes = pages * 4096u;
    hybrid::user::append_text(line, sizeof(line), cursor, "[pmap] ");
    hybrid::user::append_text(line, sizeof(line), cursor, name);
    hybrid::user::append_text(line, sizeof(line), cursor, " start=");
    hybrid::user::append_hex(line, sizeof(line), cursor, start);
    hybrid::user::append_text(line, sizeof(line), cursor, " pages=");
    hybrid::user::append_hex(line, sizeof(line), cursor, pages);
    hybrid::user::append_text(line, sizeof(line), cursor, " bytes=");
    hybrid::user::append_hex(line, sizeof(line), cursor, bytes);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::CurrentIdsInfo ids;
    auto* id_bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) id_bytes[i] = 0;
    auto id_result = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));
    if (id_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[pmap] ", "ids error ", id_result.error);
        hybrid::user::exit(1);
    }

    uint64_t pid = ids.pid;
    hybrid::ArgumentInfo arg;
    if (get_arg(1, arg)) {
        pid = parse_decimal(arg.value);
        if (pid == 0) {
            hybrid::user::write_line("[pmap] usage pmap [pid]");
            hybrid::user::exit(2);
        }
    }

    hybrid::ProcessInfo process;
    clear_process(process);
    if (!copy_process_by_pid(pid, process)) {
        hybrid::user::write_hex_line("[pmap] ", "missing pid ", pid);
        hybrid::user::exit(3);
    }

    hybrid::user::write_hex_line("[pmap] ", "pid ", process.pid);
    hybrid::user::write_text_line("[pmap] ", "name ", process.name);
    hybrid::user::write_hex_line("[pmap] ", "entry ", process.entry);
    hybrid::user::write_hex_line("[pmap] ", "pml4 ", process.address_space_root);
    write_mapping("image", process.image_base, process.image_pages);
    write_mapping("stack", process.user_stack_top - process.user_stack_pages * 4096u, process.user_stack_pages);
    hybrid::user::write_hex_line("[pmap] ", "owned pages ", process.owned_page_count);
    hybrid::user::write_hex_line("[pmap] ", "open fds ", process.open_file_count);
    hybrid::user::exit(process.pid == 0 ? 4 : 0);
}
