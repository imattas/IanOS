#include "hybrid/user.hpp"

namespace {

const char* kind_name(hybrid::FileDescriptorInfoKind kind) {
    switch (kind) {
    case hybrid::FileDescriptorInfoKind::Vfs: return "vfs";
    case hybrid::FileDescriptorInfoKind::PipeRead: return "pipe-read";
    case hybrid::FileDescriptorInfoKind::PipeWrite: return "pipe-write";
    default: return "empty";
    }
}

void write_fd_info(const hybrid::FileDescriptorInfo& info) {
    hybrid::user::write_hex_line("[fds] ", "fd ", info.fd);
    hybrid::user::write_text_line("[fds] ", "kind ", kind_name(info.kind));
    if (info.kind == hybrid::FileDescriptorInfoKind::Vfs) {
        hybrid::user::write_hex_line("[fds] ", "handle ", info.vfs_handle);
        hybrid::user::write_hex_line("[fds] ", "offset ", info.offset);
        hybrid::user::write_text_line("[fds] ", "path ", info.path);
    } else if (info.kind == hybrid::FileDescriptorInfoKind::PipeRead || info.kind == hybrid::FileDescriptorInfoKind::PipeWrite) {
        hybrid::user::write_hex_line("[fds] ", "pipe ", info.pipe_id);
    }
}

}

extern "C" [[noreturn]] void _start() {
    auto pid = hybrid::user::syscall(hybrid::SyscallNumber::GetCurrentProcessId);
    if (pid.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[fds] ", "pid error ", pid.error);
        hybrid::user::exit(1);
    }
    hybrid::user::write_hex_line("[fds] ", "pid ", pid.value);
    uint64_t count = 0;
    for (uint64_t i = 0; i < 8; ++i) {
        hybrid::FileDescriptorInfo info;
        auto* bytes = reinterpret_cast<unsigned char*>(&info);
        for (uint64_t j = 0; j < sizeof(info); ++j) bytes[j] = 0;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetFileDescriptorInfo, pid.value, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        ++count;
        write_fd_info(info);
    }
    hybrid::user::write_hex_line("[fds] ", "count ", count);
    hybrid::user::exit(count);
}
