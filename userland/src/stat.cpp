#include "hybrid/user.hpp"

namespace {

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

const char* type_name(hybrid::VfsNodeType type) {
    if (type == hybrid::VfsNodeType::Directory) return "directory";
    if (type == hybrid::VfsNodeType::MemoryFile) return "memory-file";
    if (type == hybrid::VfsNodeType::CharacterDevice) return "char-device";
    if (type == hybrid::VfsNodeType::VirtualFile) return "virtual-file";
    return "unknown";
}

}

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo path;
    if (!get_arg(1, path)) {
        hybrid::user::write_line("[stat] missing path");
        hybrid::user::exit(1);
    }

    hybrid::VfsStatInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path.value),
                                        hybrid::user::strlen(path.value) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[stat] ", "error ", result.error);
        hybrid::user::exit(2);
    }

    hybrid::user::write_text_line("[stat] ", "path ", info.path);
    hybrid::user::write_text_line("[stat] ", "type ", type_name(info.type));
    hybrid::user::write_hex_line("[stat] ", "size ", info.size);
    hybrid::user::write_hex_line("[stat] ", "links ", info.links);
    hybrid::user::write_hex_line("[stat] ", "flags ", info.flags);
    hybrid::user::exit(0);
}
