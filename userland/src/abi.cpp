#include "hybrid/user.hpp"

namespace {

void write_value(const char* label, uint64_t value) {
    hybrid::user::write_hex_line("[abi] ", label, value);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::AbiInfo info{};
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetAbiInfo, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[abi] ", "error ", result.error);
        hybrid::user::exit(1);
    }

    write_value("abi_version ", info.abi_version);
    write_value("bootinfo_version ", info.boot_info_version);
    write_value("syscall_max ", info.syscall_max_number);
    write_value("syscall_result_size ", info.syscall_result_size);
    write_value("bootinfo_size ", info.boot_info_size);
    write_value("framebuffer_size ", info.framebuffer_info_size);
    write_value("memory_region_size ", info.memory_region_size);
    write_value("boot_module_size ", info.boot_module_size);
    write_value("system_info_size ", info.system_info_size);
    write_value("limits_info_size ", info.limits_info_size);
    write_value("abi_info_size ", info.abi_info_size);
    write_value("process_info_size ", info.process_info_size);
    write_value("thread_info_size ", info.user_thread_info_size);
    write_value("vfs_node_info_size ", info.vfs_node_info_size);
    write_value("vfs_stat_info_size ", info.vfs_stat_info_size);
    write_value("mount_info_size ", info.mount_info_size);
    write_value("fd_info_size ", info.file_descriptor_info_size);
    write_value("pipe_info_size ", info.pipe_info_size);
    write_value("block_device_info_size ", info.block_device_info_size);
    write_value("feature_info_size ", info.feature_info_size);

    bool ok = info.abi_version == hybrid::kSyscallAbiVersion &&
        info.syscall_max_number == hybrid::kSyscallMaxNumber &&
        info.boot_info_size == sizeof(hybrid::BootInfo) &&
        info.abi_info_size == sizeof(hybrid::AbiInfo) &&
        info.feature_info_size == sizeof(hybrid::FeatureInfo) &&
        info.system_info_size == sizeof(hybrid::SystemInfo) &&
        info.limits_info_size == sizeof(hybrid::LimitsInfo);
    hybrid::user::exit(ok ? 0 : 2);
}
