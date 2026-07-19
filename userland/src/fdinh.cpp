#include "hybrid/user.hpp"

namespace {

void write_all(uint32_t fd, const char* text) {
    uint64_t length = hybrid::user::strlen(text);
    uint64_t written = 0;
    while (written < length) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile, fd, reinterpret_cast<uint64_t>(text + written), length - written);
        if (result.error != hybrid::kSyscallErrorNone || result.value == 0) hybrid::user::exit(2);
        written += result.value;
    }
}

void clear_fd(hybrid::FileDescriptorInfo& info) {
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    static const char input_path[] = "/tmp/fdinherit.in";
    static const char output_path[] = "/tmp/fdinherit.out";
    static const char payload[] = "shared-offset\n";
    static const char cat_command[] = "/bin/cat.elf";

    hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(input_path), sizeof(input_path));
    hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(output_path), sizeof(output_path));
    if (hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(input_path), sizeof(input_path)).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] create input error");
        hybrid::user::exit(1);
    }
    if (hybrid::user::syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(output_path), sizeof(output_path)).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] create output error");
        hybrid::user::exit(1);
    }

    auto input = hybrid::user::syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(input_path), sizeof(input_path));
    if (input.error != hybrid::kSyscallErrorNone || input.value < 3) {
        hybrid::user::write_hex_line("[fdinh] ", "open input error ", input.error);
        hybrid::user::exit(1);
    }
    write_all(static_cast<uint32_t>(input.value), payload);
    if (hybrid::user::syscall(hybrid::SyscallNumber::Seek, input.value, 0).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] seek input error");
        hybrid::user::exit(1);
    }
    if (hybrid::user::syscall(hybrid::SyscallNumber::Dup2, input.value, hybrid::kStdinFd).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] dup2 stdin error");
        hybrid::user::exit(1);
    }

    uint64_t pid = 0;
    auto spawned = hybrid::user::syscall(hybrid::SyscallNumber::Spawn,
                                         reinterpret_cast<uint64_t>(cat_command),
                                         sizeof(cat_command),
                                         reinterpret_cast<uint64_t>(&pid),
                                         hybrid::SpawnFlagStartSuspended);
    if (spawned.error != hybrid::kSyscallErrorNone || pid == 0) {
        hybrid::user::write_hex_line("[fdinh] ", "spawn error ", spawned.error);
        hybrid::user::exit(1);
    }
    if (hybrid::user::syscall(hybrid::SyscallNumber::RedirectProcessFd, pid, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(output_path), sizeof(output_path)).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] redirect child error");
        hybrid::user::exit(1);
    }
    if (hybrid::user::syscall(hybrid::SyscallNumber::StartProcess, pid).error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_line("[fdinh] start child error");
        hybrid::user::exit(1);
    }

    uint64_t code = 0;
    for (;;) {
        auto waited = hybrid::user::syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::ReapProcess, pid);

    hybrid::FileDescriptorInfo info;
    bool found_stdin = false;
    for (uint64_t i = 0; i < 8; ++i) {
        clear_fd(info);
        auto fdinfo = hybrid::user::syscall(hybrid::SyscallNumber::GetFileDescriptorInfo, 0, i, reinterpret_cast<uint64_t>(&info));
        if (fdinfo.error == hybrid::kSyscallErrorNone && info.fd == hybrid::kStdinFd) {
            found_stdin = true;
            break;
        }
    }
    if (!found_stdin) {
        hybrid::user::write_line("[fdinh] fdinfo stdin missing");
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[fdinh] ", "child code ", code);
    hybrid::user::write_hex_line("[fdinh] ", "stdin offset ", info.offset);
    hybrid::user::write_hex_line("[fdinh] ", "expected ", sizeof(payload) - 1);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, input.value);
    hybrid::user::exit(info.offset == sizeof(payload) - 1 ? 0 : 3);
}
