#include "hybrid/user.hpp"

namespace {

void clear_pipe_info(hybrid::PipeInfo& info) {
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
}

void write_pipe_info(const hybrid::PipeInfo& info) {
    hybrid::user::write_hex_line("[pipeinfo] ", "id ", info.id);
    hybrid::user::write_hex_line("[pipeinfo] ", "size ", info.size);
    hybrid::user::write_hex_line("[pipeinfo] ", "capacity ", info.capacity);
    hybrid::user::write_hex_line("[pipeinfo] ", "read offset ", info.read_offset);
    hybrid::user::write_hex_line("[pipeinfo] ", "readers ", info.reader_count);
    hybrid::user::write_hex_line("[pipeinfo] ", "writers ", info.writer_count);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetPipeCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[pipeinfo] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[pipeinfo] ", "count ", count.value);
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::PipeInfo info;
        clear_pipe_info(info);
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetPipeInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[pipeinfo] ", "info error ", result.error);
            hybrid::user::exit(2);
        }
        write_pipe_info(info);
    }

    hybrid::user::exit(0);
}
