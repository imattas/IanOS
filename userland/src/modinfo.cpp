#include "hybrid/user.hpp"

namespace {

struct ModuleRow {
    char name[40];
    char size[24];
    char address[24];
    char path[64];
};

struct Buffer {
    char bytes[8192];
    uint64_t length;
};

Buffer g_modules;
char g_chunk[64];

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    clear(&out, sizeof(out));
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

bool text_equals(const char* left, const char* right) {
    if (!left || !right) return false;
    uint64_t i = 0;
    for (; left[i] != 0 && right[i] != 0; ++i) {
        if (left[i] != right[i]) return false;
    }
    return left[i] == 0 && right[i] == 0;
}

const char* basename_of(const char* path) {
    const char* name = path;
    if (!path) return "";
    for (uint64_t i = 0; path[i] != 0; ++i) {
        if (path[i] == '/' && path[i + 1] != 0) name = path + i + 1;
    }
    return name;
}

void copy_token(char* out, uint64_t capacity, const char* line, uint64_t& cursor, uint64_t end) {
    if (capacity == 0) return;
    while (cursor < end && line[cursor] == ' ') ++cursor;
    uint64_t out_cursor = 0;
    while (cursor < end && line[cursor] != ' ') {
        if (out_cursor + 1 < capacity) out[out_cursor++] = line[cursor];
        ++cursor;
    }
    out[out_cursor] = 0;
}

bool parse_module_line(const char* line, uint64_t length, ModuleRow& out) {
    clear(&out, sizeof(out));
    uint64_t cursor = 0;
    copy_token(out.name, sizeof(out.name), line, cursor, length);
    copy_token(out.size, sizeof(out.size), line, cursor, length);
    copy_token(out.address, sizeof(out.address), line, cursor, length);
    copy_token(out.path, sizeof(out.path), line, cursor, length);
    return out.name[0] != 0 && out.size[0] != 0 && out.address[0] != 0 && out.path[0] != 0;
}

bool read_modules(Buffer& out) {
    out.length = 0;
    const char* path = "/proc/modules";
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(path), hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        hybrid::user::write_hex_line("[modinfo] ", "open error ", opened.error);
        return false;
    }

    while (out.length < sizeof(out.bytes)) {
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(g_chunk), sizeof(g_chunk));
        if (read.error != hybrid::kSyscallErrorNone) {
            if (read.error == hybrid::kSyscallErrorNotFound && out.length != 0) break;
            hybrid::user::write_hex_line("[modinfo] ", "read error ", read.error);
            hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
            return false;
        }
        if (read.value == 0) break;
        for (uint64_t i = 0; i < read.value && out.length < sizeof(out.bytes); ++i) {
            out.bytes[out.length++] = g_chunk[i];
        }
    }

    hybrid::user::syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    return out.length != 0;
}

bool row_matches(const ModuleRow& row, const char* query) {
    return text_equals(row.name, query) || text_equals(row.path, query) || text_equals(basename_of(row.path), query);
}

bool find_module(const Buffer& buffer, const char* query, ModuleRow& out) {
    uint64_t start = 0;
    uint64_t row_index = 0;
    for (uint64_t i = 0; i <= buffer.length; ++i) {
        if (i != buffer.length && buffer.bytes[i] != '\n') continue;
        uint64_t length = i - start;
        if (length != 0 && buffer.bytes[start + length - 1] == '\r') --length;
        if (length != 0 && row_index != 0) {
            ModuleRow row;
            if (parse_module_line(buffer.bytes + start, length, row) && row_matches(row, query)) {
                out = row;
                return true;
            }
        }
        ++row_index;
        start = i + 1;
    }
    return false;
}

void print_row(const ModuleRow& row) {
    hybrid::user::write_text_line("[modinfo] ", "name ", row.name);
    hybrid::user::write_text_line("[modinfo] ", "path ", row.path);
    hybrid::user::write_text_line("[modinfo] ", "size ", row.size);
    hybrid::user::write_text_line("[modinfo] ", "address ", row.address);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo query;
    if (!get_arg(1, query)) {
        hybrid::user::write_line("[modinfo] usage modinfo <module-name|path>");
        hybrid::user::exit(1);
    }
    if (!read_modules(g_modules)) hybrid::user::exit(1);

    ModuleRow row;
    if (!find_module(g_modules, query.value, row)) {
        hybrid::user::write_text_line("[modinfo] ", "missing ", query.value);
        hybrid::user::exit(2);
    }

    print_row(row);
    hybrid::user::exit(0);
}
