#include "hybrid/syscall.hpp"

namespace {

hybrid::SyscallResult syscall(hybrid::SyscallNumber number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0) {
    register uint64_t rax asm("rax") = static_cast<uint64_t>(number);
    register uint64_t rdi asm("rdi") = arg0;
    register uint64_t rsi asm("rsi") = arg1;
    register uint64_t rdx asm("rdx") = arg2;
    register uint64_t r10 asm("r10") = arg3;
    asm volatile(
        "int $0x80"
        : "+a"(rax), "+d"(rdx)
        : "D"(rdi), "S"(rsi), "r"(r10)
        : "rcx", "r8", "r9", "r11", "memory");
    return {rax, rdx};
}

uint64_t strlen(const char* text) {
    uint64_t length = 0;
    while (text[length] != 0) ++length;
    return length;
}

void write_text(const char* text) {
    syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(text), strlen(text));
}

void write_line(const char* text) {
    write_text(text);
    write_text("\n");
}

char hex_digit(uint64_t value) {
    value &= 0xf;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

void append_char(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
}

void append_text(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) append_char(buffer, capacity, cursor, text[i]);
}

void append_hex(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    append_text(buffer, capacity, cursor, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) append_char(buffer, capacity, cursor, hex_digit(value >> shift));
}

void clear_node(hybrid::VfsDirectoryEntryInfo& node) {
    node.type = hybrid::VfsNodeType::Empty;
    node.flags = 0;
    node.size = 0;
    node.links = 0;
    for (uint64_t i = 0; i < sizeof(node.name); ++i) node.name[i] = 0;
    for (uint64_t i = 0; i < sizeof(node.path); ++i) node.path[i] = 0;
}

void write_count(uint64_t count) {
    char line[64];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[ls] count ");
    append_hex(line, sizeof(line), cursor, count);
    write_line(line);
}

void write_node(const hybrid::VfsDirectoryEntryInfo& node) {
    char line[128];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[ls] ");
    append_text(line, sizeof(line), cursor,
                node.type == hybrid::VfsNodeType::Directory ? "dir " :
                (node.type == hybrid::VfsNodeType::CharacterDevice ? "char " :
                (node.type == hybrid::VfsNodeType::VirtualFile ? "virt " : "file ")));
    append_text(line, sizeof(line), cursor, node.path);
    write_line(line);
}

void clear_argument(hybrid::ArgumentInfo& argument) {
    for (uint64_t i = 0; i < sizeof(argument.value); ++i) argument.value[i] = 0;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
    for (++i; i < capacity; ++i) out[i] = 0;
}

void entry_from_stat(const hybrid::VfsStatInfo& stat, hybrid::VfsDirectoryEntryInfo& out) {
    out.type = stat.type;
    out.flags = stat.flags;
    out.size = stat.size;
    out.links = stat.links;
    copy_text(out.path, sizeof(out.path), stat.path);
    const char* name = stat.path;
    for (uint64_t i = 0; stat.path[i] != 0; ++i) {
        if (stat.path[i] == '/' && stat.path[i + 1] != 0) name = stat.path + i + 1;
    }
    copy_text(out.name, sizeof(out.name), name);
}

bool read_directory_entry(const char* path, uint64_t index, hybrid::VfsDirectoryEntryInfo& out) {
    clear_node(out);
    auto result = syscall(hybrid::SyscallNumber::ReadDirectory,
                          reinterpret_cast<uint64_t>(path),
                          strlen(path) + 1,
                          index,
                          reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone && result.value == 1;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo target;
    clear_argument(target);
    auto argc = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argc.error == hybrid::kSyscallErrorNone && argc.value > 1) {
        syscall(hybrid::SyscallNumber::GetArgument, 1, reinterpret_cast<uint64_t>(&target));
    } else {
        hybrid::PathInfo cwd;
        auto cwd_result = syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
        copy_text(target.value, sizeof(target.value), cwd_result.error == hybrid::kSyscallErrorNone ? cwd.path : "/");
    }

    hybrid::VfsStatInfo stat;
    auto stat_result = syscall(hybrid::SyscallNumber::VfsStatInfo,
                               reinterpret_cast<uint64_t>(target.value),
                               strlen(target.value) + 1,
                               reinterpret_cast<uint64_t>(&stat));
    if (stat_result.error != hybrid::kSyscallErrorNone || stat_result.value != 1) {
        write_line("[ls] path error");
        syscall(hybrid::SyscallNumber::Exit, 1);
    }

    if (stat.type != hybrid::VfsNodeType::Directory) {
        hybrid::VfsDirectoryEntryInfo single;
        clear_node(single);
        entry_from_stat(stat, single);
        write_count(1);
        write_node(single);
        syscall(hybrid::SyscallNumber::Exit, 1);
    }

    uint64_t entries = 0;
    for (;;) {
        hybrid::VfsDirectoryEntryInfo entry;
        if (!read_directory_entry(target.value, entries, entry)) break;
        ++entries;
    }
    write_count(entries);
    for (uint64_t i = 0; i < entries; ++i) {
        hybrid::VfsDirectoryEntryInfo entry;
        if (read_directory_entry(target.value, i, entry)) write_node(entry);
    }
    syscall(hybrid::SyscallNumber::Exit, entries);
    for (;;) asm volatile("pause");
}
