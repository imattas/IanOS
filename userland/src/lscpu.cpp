#include "hybrid/user.hpp"

namespace {

void clear(void* ptr, uint64_t size) {
    auto* bytes = reinterpret_cast<unsigned char*>(ptr);
    for (uint64_t i = 0; i < size; ++i) bytes[i] = 0;
}

void write_cpu(const hybrid::CpuInfo& info) {
    char line[192];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, "[lscpu] cpu ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.cpu_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " apic ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.apic_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " acpi ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.acpi_processor_id);
    hybrid::user::append_text(line, sizeof(line), cursor, " flags ");
    hybrid::user::append_hex(line, sizeof(line), cursor, info.flags);
    hybrid::user::write_line(line);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetCpuCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[lscpu] ", "count error ", count.error);
        hybrid::user::exit(1);
    }

    hybrid::user::write_hex_line("[lscpu] ", "cpus ", count.value);
    uint64_t online = 0;
    uint64_t parked = 0;
    uint64_t schedulers = 0;
    uint64_t timers = 0;
    for (uint64_t i = 0; i < count.value && i < 64; ++i) {
        hybrid::CpuInfo info;
        clear(&info, sizeof(info));
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetCpuInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) {
            hybrid::user::write_hex_line("[lscpu] ", "info error ", result.error);
            hybrid::user::exit(2);
        }
        if ((info.flags & hybrid::CpuInfoOnline) != 0) ++online;
        if ((info.flags & hybrid::CpuInfoParked) != 0) ++parked;
        if ((info.flags & hybrid::CpuInfoScheduler) != 0) ++schedulers;
        if ((info.flags & hybrid::CpuInfoLocalApicTimerReady) != 0) ++timers;
        write_cpu(info);
    }

    hybrid::user::write_hex_line("[lscpu] ", "online ", online);
    hybrid::user::write_hex_line("[lscpu] ", "parked ", parked);
    hybrid::user::write_hex_line("[lscpu] ", "schedulers ", schedulers);
    hybrid::user::write_hex_line("[lscpu] ", "lapic timers ", timers);
    hybrid::user::exit(0);
}
