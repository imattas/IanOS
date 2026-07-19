#include "hybrid/user.hpp"

namespace {

bool stat_char_device(const char* path) {
    hybrid::VfsStatInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone || result.value != 1 ||
        info.type != hybrid::VfsNodeType::CharacterDevice) {
        hybrid::user::write_text_line("[tty] ", "stat error ", path);
        return false;
    }
    hybrid::user::write_text_line("[tty] ", "path ", info.path);
    hybrid::user::write_text_line("[tty] ", "type ", "char-device");
    return true;
}

bool write_device(const char* path, const char* payload) {
    auto fd = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                    reinterpret_cast<uint64_t>(path),
                                    hybrid::user::strlen(path) + 1);
    if (fd.error != hybrid::kSyscallErrorNone || fd.value < 3) {
        hybrid::user::write_text_line("[tty] ", "open error ", path);
        return false;
    }
    auto wrote = hybrid::user::syscall(hybrid::SyscallNumber::WriteFile,
                                       fd.value,
                                       reinterpret_cast<uint64_t>(payload),
                                       hybrid::user::strlen(payload));
    hybrid::user::syscall(hybrid::SyscallNumber::Close, fd.value);
    if (wrote.error != hybrid::kSyscallErrorNone || wrote.value != hybrid::user::strlen(payload)) {
        hybrid::user::write_text_line("[tty] ", "write error ", path);
        hybrid::user::write_hex_line("[tty] ", "write bytes ", wrote.value);
        return false;
    }
    hybrid::user::write_text_line("[tty] ", "write ", path);
    return true;
}

bool read_empty_device(const char* path) {
    auto fd = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                    reinterpret_cast<uint64_t>(path),
                                    hybrid::user::strlen(path) + 1);
    if (fd.error != hybrid::kSyscallErrorNone || fd.value < 3) {
        hybrid::user::write_text_line("[tty] ", "read open error ", path);
        return false;
    }
    char byte = 0;
    auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read,
                                      fd.value,
                                      reinterpret_cast<uint64_t>(&byte),
                                      1);
    hybrid::user::syscall(hybrid::SyscallNumber::Close, fd.value);
    if (read.error != hybrid::kSyscallErrorNotFound || read.value != 0) {
        hybrid::user::write_text_line("[tty] ", "read idle error ", path);
        hybrid::user::write_hex_line("[tty] ", "read bytes ", read.value);
        return false;
    }
    hybrid::user::write_text_line("[tty] ", "read idle ", path);
    return true;
}

bool verify_input_mode() {
    auto mode = hybrid::user::syscall(hybrid::SyscallNumber::TerminalControl,
                                      static_cast<uint64_t>(hybrid::TerminalControlCommand::GetInputMode));
    if (mode.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[tty] ", "mode error ", mode.error);
        return false;
    }
    if (mode.value == static_cast<uint64_t>(hybrid::TerminalInputMode::Raw)) {
        hybrid::user::write_line("[tty] mode raw");
        return true;
    }
    if (mode.value == static_cast<uint64_t>(hybrid::TerminalInputMode::Canonical)) {
        hybrid::user::write_line("[tty] mode canonical");
        return true;
    }
    hybrid::user::write_hex_line("[tty] ", "mode unknown ", mode.value);
    return false;
}

}

extern "C" [[noreturn]] void _start() {
    bool ok = true;
    ok = verify_input_mode() && ok;
    ok = stat_char_device("/dev/tty") && ok;
    ok = read_empty_device("/dev/tty") && ok;
    ok = write_device("/dev/tty", "[tty] hello tty\n") && ok;
    ok = stat_char_device("/dev/console") && ok;
    ok = read_empty_device("/dev/console") && ok;
    ok = write_device("/dev/console", "[tty] hello console\n") && ok;
    if (ok) hybrid::user::write_line("[tty] terminal devices ok");
    hybrid::user::exit(ok ? 0 : 1);
}
