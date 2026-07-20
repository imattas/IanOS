#include "hybrid/user.hpp"

namespace {

void write_value(const char* label, uint64_t value) {
    hybrid::user::write_hex_line("[limits] ", label, value);
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::LimitsInfo limits{};
    auto limits_result = hybrid::user::syscall(hybrid::SyscallNumber::GetLimitsInfo, reinterpret_cast<uint64_t>(&limits));
    if (limits_result.error != hybrid::kSyscallErrorNone) {
        hybrid::user::write_hex_line("[limits] ", "error ", limits_result.error);
        hybrid::user::exit(1);
    }

    auto nodes = hybrid::user::syscall(hybrid::SyscallNumber::GetVfsNodeCount);
    auto pipes = hybrid::user::syscall(hybrid::SyscallNumber::GetPipeCount);
    auto mounts = hybrid::user::syscall(hybrid::SyscallNumber::GetMountCount);
    auto processes = hybrid::user::syscall(hybrid::SyscallNumber::GetProcessCount);
    auto threads = hybrid::user::syscall(hybrid::SyscallNumber::GetUserThreadCount);

    write_value("boot_modules ", limits.max_boot_modules);
    write_value("vfs_nodes ", limits.max_vfs_nodes);
    if (nodes.error == hybrid::kSyscallErrorNone) write_value("vfs_nodes_used ", nodes.value);
    write_value("file_handles ", limits.max_file_handles);
    write_value("ram_files ", limits.max_ram_files);
    write_value("ram_directories ", limits.max_ram_directories);
    write_value("ram_links ", limits.max_ram_links);
    write_value("mounts ", limits.max_mounts);
    if (mounts.error == hybrid::kSyscallErrorNone) write_value("mounts_used ", mounts.value);
    write_value("ram_file_bytes ", limits.max_ram_file_bytes);
    write_value("process_slots ", limits.max_user_processes);
    if (processes.error == hybrid::kSyscallErrorNone) write_value("processes_created ", processes.value);
    write_value("user_thread_slots ", limits.max_user_threads);
    if (threads.error == hybrid::kSyscallErrorNone) write_value("user_threads ", threads.value);
    write_value("process_fds ", limits.max_process_file_descriptors);
    write_value("owned_user_pages ", limits.max_owned_user_pages);
    write_value("argv_entries ", limits.max_process_arguments);
    write_value("argv_bytes ", limits.max_argument_length);
    write_value("env_entries ", limits.max_environment_entries);
    write_value("env_key_bytes ", limits.max_environment_key_length);
    write_value("env_value_bytes ", limits.max_environment_value_length);
    write_value("pipes ", limits.max_pipes);
    if (pipes.error == hybrid::kSyscallErrorNone) write_value("pipes_used ", pipes.value);
    write_value("pipe_bytes ", limits.pipe_capacity);
    write_value("cpus ", limits.max_cpus);
    write_value("pmm_bitmap_pages ", limits.pmm_bitmap_pages);
    write_value("fat_paths ", limits.mounted_fat_path_capacity);

    hybrid::user::exit(limits.max_vfs_nodes != 0 && limits.max_user_processes != 0 &&
        limits.max_user_threads != 0 && limits.max_process_file_descriptors >= 3 ? 0 : 2);
}
