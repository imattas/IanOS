#include "hybrid/user.hpp"

extern "C" [[noreturn]] void _start() {
    hybrid::BlockDeviceInfo info;
    auto info_result = hybrid::user::syscall(hybrid::SyscallNumber::GetBlockDeviceInfo, reinterpret_cast<uint64_t>(&info));
    if (info_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[blk] ", "info error ", info_result.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[blk] ", "initialized ", info.initialized);
    hybrid::user::write_hex_line("[blk] ", "sector size ", info.sector_size);
    hybrid::user::write_hex_line("[blk] ", "reads ", info.sector_reads);
    hybrid::user::write_hex_line("[blk] ", "cache hits ", info.cache_hits);
    hybrid::user::write_hex_line("[blk] ", "cache misses ", info.cache_misses);
    hybrid::user::write_hex_line("[blk] ", "cache fills ", info.cache_fills);
    hybrid::user::write_hex_line("[blk] ", "cache entries ", info.cached_entries);
    hybrid::user::write_hex_line("[blk] ", "invalid reads ", info.invalid_reads);
    hybrid::user::write_hex_line("[blk] ", "backend failures ", info.backend_read_failures);

    unsigned char sector[512];
    auto read_result = hybrid::user::syscall(hybrid::SyscallNumber::ReadBlockSector, 0, reinterpret_cast<uint64_t>(sector));
    if (read_result.error != hybrid::kSyscallErrorNone || read_result.value != sizeof(sector)) {
        hybrid::user::write_hex_line("[blk] ", "read error ", read_result.error);
        hybrid::user::write_hex_line("[blk] ", "read bytes ", read_result.value);
        hybrid::user::exit(2);
    }
    uint64_t signature = static_cast<uint64_t>(sector[510]) | (static_cast<uint64_t>(sector[511]) << 8);
    hybrid::user::write_hex_line("[blk] ", "lba0 bytes ", read_result.value);
    hybrid::user::write_hex_line("[blk] ", "lba0 signature ", signature);
    hybrid::user::write_text("[blk] oem ");
    for (uint64_t i = 3; i < 11 && sector[i] != 0; ++i) {
        char ch[2] = {static_cast<char>(sector[i]), 0};
        hybrid::user::write_text(ch);
    }
    hybrid::user::write_text("\n");
    hybrid::user::exit(signature == 0xaa55 ? 0 : 3);
}
