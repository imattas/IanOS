#include "hk/syscall/syscall.hpp"
#include "hybrid/version.hpp"
#include "hk/log.hpp"
#include "hk/sched/scheduler.hpp"
#include "hk/timer/pit.hpp"
#include "hk/userspace/userspace.hpp"
#include "hk/fs/vfs.hpp"
#include "hk/drivers/device_inventory.hpp"
#include "hk/boot/bootinfo.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/drivers/ps2_keyboard.hpp"
#include "hk/terminal.hpp"
#include "hk/time/rtc.hpp"
#include "hk/cpu/topology.hpp"
#include "hk/cpu/runtime.hpp"
#include "hk/smp/smp.hpp"
#include "hk/block/block_device.hpp"
#include "hk/lib/string.hpp"

namespace hk::syscall {

namespace {
constexpr uint64_t kMaxSpawnCommandLine = 128;

bool c_string_readable(const char* text, uint64_t max_len) {
    if (!text || max_len == 0 || max_len > 4096) return false;
    for (uint64_t i = 0; i < max_len; ++i) {
        if (text[i] == 0) return true;
    }
    return false;
}

bool user_buffer_readable(const char* data, uint64_t length) {
    if (!data || length == 0 || length > 4096) return false;
    for (uint64_t i = 0; i < length; ++i) {
        volatile char c = data[i];
        (void)c;
    }
    return true;
}

bool user_buffer_writable(char* data, uint64_t length) {
    if (!data || length == 0 || length > 4096) return false;
    volatile char c = data[0];
    data[0] = c;
    return true;
}

bool copy_spawn_command(const char* text, uint64_t length, char (&out)[kMaxSpawnCommandLine]) {
    if (!c_string_readable(text, length) || length > sizeof(out)) return false;
    for (uint64_t i = 0; i < sizeof(out); ++i) out[i] = 0;
    for (uint64_t i = 0; i < length; ++i) {
        out[i] = text[i];
        if (text[i] == 0) return true;
    }
    return false;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    out[i] = 0;
}

uint32_t parse_spawn_arguments(char* command, const char** args, uint32_t max_args) {
    uint32_t count = 0;
    char* cursor = command;
    while (*cursor != 0 && count < max_args) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == 0) break;
        char* out = cursor;
        args[count++] = out;
        char quote = 0;
        while (*cursor != 0) {
            char c = *cursor++;
            if (quote == 0 && (c == ' ' || c == '\t')) break;
            if (quote == 0 && (c == '\'' || c == '"')) {
                quote = c;
                continue;
            }
            if (quote != 0 && c == quote) {
                quote = 0;
                continue;
            }
            if (quote != '\'' && c == '\\' && *cursor != 0) {
                c = *cursor++;
            }
            *out++ = c;
        }
        *out = 0;
    }
    return count;
}

bool auto_enable_user_preemption_if_competing() {
    return hk::userspace::userspace_manager().update_user_preemption_gate();
}

uint64_t syscall_current_pid() {
    return hk::userspace::userspace_manager().current_pid();
}

uint64_t syscall_current_tid() {
    return hk::userspace::userspace_manager().current_tid();
}

bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    for (uint64_t i = 0; prefix[i] != 0; ++i) {
        if (text[i] != prefix[i]) return false;
    }
    return true;
}

bool parse_decimal_tail(const char* text, uint32_t& out) {
    if (!text || text[0] == 0) return false;
    uint64_t value = 0;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        value = value * 10 + static_cast<uint64_t>(text[i] - '0');
        if (value > 0xffffffffull) return false;
    }
    out = static_cast<uint32_t>(value);
    return true;
}

uint64_t copy_link_target(char* out, uint64_t capacity, const char* target) {
    if (!out || capacity == 0 || !target) return 0;
    uint64_t i = 0;
    for (; i + 1 < capacity && target[i] != 0; ++i) out[i] = target[i];
    out[i] = 0;
    return i;
}

bool proc_fd_target(uint64_t pid, uint32_t fd, hybrid::FileDescriptorInfo& out) {
    if (pid == 0) return false;
    auto& manager = hk::userspace::userspace_manager();
    for (uint64_t i = 0; i < hk::userspace::kMaxProcessFileDescriptors; ++i) {
        hybrid::FileDescriptorInfo info{};
        if (!manager.copy_file_descriptor_info(pid, i, info) || info.fd != fd) continue;
        out = info;
        return true;
    }
    return false;
}

bool proc_self_fd_target(uint32_t fd, hybrid::FileDescriptorInfo& out) {
    return proc_fd_target(syscall_current_pid(), fd, out);
}

bool parse_proc_pid_fd_path(const char* path, uint64_t& pid, uint32_t& fd) {
    pid = 0;
    fd = 0;
    constexpr const char* kProcPrefix = "/proc/";
    if (!starts_with(path, kProcPrefix)) return false;
    uint64_t cursor = 6;
    if (path[cursor] < '0' || path[cursor] > '9') return false;
    while (path[cursor] >= '0' && path[cursor] <= '9') {
        pid = pid * 10 + static_cast<uint64_t>(path[cursor] - '0');
        ++cursor;
    }
    constexpr const char* kFdSegment = "/fd/";
    for (uint64_t i = 0; kFdSegment[i] != 0; ++i) {
        if (path[cursor + i] != kFdSegment[i]) return false;
    }
    cursor += 4;
    return parse_decimal_tail(path + cursor, fd);
}

Result link_target_result(char* out, uint64_t capacity, const hybrid::FileDescriptorInfo& info) {
    if (info.kind == hybrid::FileDescriptorInfoKind::Vfs) {
        return {copy_link_target(out, capacity, info.path), kErrorNone};
    }
    char pipe_target[32];
    for (uint64_t i = 0; i < sizeof(pipe_target); ++i) pipe_target[i] = 0;
    copy_text(pipe_target, sizeof(pipe_target), info.kind == hybrid::FileDescriptorInfoKind::PipeRead ? "pipe-read:" : "pipe-write:");
    uint64_t cursor = 0;
    while (pipe_target[cursor] != 0 && cursor + 1 < sizeof(pipe_target)) ++cursor;
    char digits[11];
    uint64_t count = 0;
    uint32_t value = info.pipe_id;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count > 0 && cursor + 1 < sizeof(pipe_target)) pipe_target[cursor++] = digits[--count];
    pipe_target[cursor] = 0;
    return {copy_link_target(out, capacity, pipe_target), kErrorNone};
}
}

Result dispatch(uint64_t number, uint64_t arg0, uint64_t arg1, uint64_t arg2, uint64_t arg3) {
    hk::userspace::userspace_manager().record_current_syscall(number);
    switch (static_cast<Number>(number)) {
    case Number::DebugLog: {
        auto* text = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(text, arg1)) return {0, kErrorInvalidPointer};
        hk::log(hk::LogLevel::Info, text);
        return {0, kErrorNone};
    }
    case Number::GetTicks:
        return {hk::timer::ticks(), kErrorNone};
    case Number::GetDateTime: {
        auto* out = reinterpret_cast<hybrid::DateTimeInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        return hk::time::read_rtc_datetime(*out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Yield:
        hk::sched::yield();
        return {0, kErrorNone};
    case Number::SleepTicks:
        return {0, kErrorNone};
    case Number::GetThreadId: {
        auto* current = hk::sched::scheduler().current_thread();
        return {current ? current->id : 0, kErrorNone};
    }
    case Number::GetProcessCount:
        return {hk::userspace::userspace_manager().process_count(), kErrorNone};
    case Number::GetThreadCount:
        return {hk::sched::scheduler().thread_count(), kErrorNone};
    case Number::GetRunnableProcessCount:
        return {hk::userspace::userspace_manager().runnable_count(), kErrorNone};
    case Number::GetExitedProcessCount:
        return {hk::userspace::userspace_manager().exited_count(), kErrorNone};
    case Number::GetUserThreadCount:
        return {hk::userspace::userspace_manager().user_thread_count(), kErrorNone};
    case Number::GetRunnableUserThreadCount:
        return {hk::userspace::userspace_manager().runnable_thread_count(), kErrorNone};
    case Number::GetLiveProcessCount:
        return {hk::userspace::userspace_manager().live_process_count(), kErrorNone};
    case Number::GetCpuCount:
        return {hk::cpu::topology().cpu_count(), kErrorNone};
    case Number::ReapProcess:
        return {hk::userspace::userspace_manager().reap_exited(arg0) ? 1ull : 0ull, kErrorNone};
    case Number::VfsStat: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        const auto* node = hk::fs::vfs().find(path);
        if (!node || node->type != hk::fs::NodeType::MemoryFile) return {0, kErrorNotFound};
        return {node->ram_file ? node->ram_file->size : node->size, kErrorNone};
    }
    case Number::VfsStatInfo: {
        auto* path = reinterpret_cast<const char*>(arg0);
        auto* out = reinterpret_cast<hybrid::VfsStatInfo*>(arg2);
        if (!c_string_readable(path, arg1) || !out) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid != 0) {
            return hk::userspace::userspace_manager().stat_path(pid, path, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
        }
        return hk::fs::vfs().stat(path, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::VfsRead: {
        auto* path = reinterpret_cast<const char*>(arg0);
        auto* buffer = reinterpret_cast<void*>(arg2);
        if (!c_string_readable(path, arg1) || !buffer || arg3 == 0 || arg3 > 4096) return {0, kErrorInvalidPointer};
        const auto* node = hk::fs::vfs().find(path);
        if (!node || node->type != hk::fs::NodeType::MemoryFile) return {0, kErrorNotFound};
        return {hk::fs::vfs().read(path, 0, buffer, static_cast<size_t>(arg3)), kErrorNone};
    }
    case Number::VfsOpen: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint32_t handle = hk::fs::vfs().open(path);
        return handle == 0 ? Result{0, kErrorNotFound} : Result{handle, kErrorNone};
    }
    case Number::VfsReadHandle: {
        auto* buffer = reinterpret_cast<void*>(arg1);
        if (!buffer || arg2 == 0 || arg2 > 4096) return {0, kErrorInvalidPointer};
        size_t bytes = hk::fs::vfs().read_handle(static_cast<uint32_t>(arg0), buffer, static_cast<size_t>(arg2));
        return bytes == 0 ? Result{0, kErrorNotFound} : Result{bytes, kErrorNone};
    }
    case Number::VfsClose:
        return hk::fs::vfs().close(static_cast<uint32_t>(arg0)) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    case Number::Open: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        uint32_t fd = hk::userspace::userspace_manager().open_file(pid, path);
        return fd == 0 ? Result{0, kErrorNotFound} : Result{fd, kErrorNone};
    }
    case Number::Read: {
        auto* buffer = reinterpret_cast<char*>(arg1);
        if (!user_buffer_writable(buffer, arg2)) return {0, kErrorInvalidPointer};
        if (arg0 == hybrid::kStdinFd) {
            uint64_t pid = syscall_current_pid();
            if (pid != 0 && hk::userspace::userspace_manager().has_open_file(pid, hybrid::kStdinFd)) {
                uint64_t bytes = hk::userspace::userspace_manager().read_file(pid, hybrid::kStdinFd, buffer, arg2);
                if (bytes != 0) return {bytes, kErrorNone};
                if (hk::userspace::userspace_manager().pipe_read_would_block(pid, hybrid::kStdinFd)) return {0, kErrorWouldBlock};
                if (hk::userspace::userspace_manager().is_pipe_fd(pid, hybrid::kStdinFd)) return {0, kErrorNone};
                return {0, kErrorNotFound};
            }
            uint64_t bytes = 0;
            bytes = hk::terminal::read_input(buffer, static_cast<size_t>(arg2));
            return bytes == 0 ? Result{0, kErrorNotFound} : Result{bytes, kErrorNone};
        }
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        uint64_t bytes = hk::userspace::userspace_manager().read_file(pid, static_cast<uint32_t>(arg0), buffer, arg2);
        if (bytes != 0) return {bytes, kErrorNone};
        if (hk::userspace::userspace_manager().pipe_read_would_block(pid, static_cast<uint32_t>(arg0))) return {0, kErrorWouldBlock};
        if (hk::userspace::userspace_manager().is_pipe_fd(pid, static_cast<uint32_t>(arg0))) return {0, kErrorNone};
        return {0, kErrorNotFound};
    }
    case Number::Close: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().close_file(pid, static_cast<uint32_t>(arg0)) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Dup: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        uint32_t fd = hk::userspace::userspace_manager().duplicate_file(pid, static_cast<uint32_t>(arg0));
        return fd == 0 ? Result{0, kErrorNotFound} : Result{fd, kErrorNone};
    }
    case Number::Dup2: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().duplicate_file_to(pid, static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1))
            ? Result{arg1, kErrorNone}
            : Result{0, kErrorNotFound};
    }
    case Number::Seek: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().seek_file(pid, static_cast<uint32_t>(arg0), arg1) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::CreateFile: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().create_file(pid, path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::WriteFile: {
        auto* buffer = reinterpret_cast<const char*>(arg1);
        if (!user_buffer_readable(buffer, arg2)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        uint64_t bytes = hk::userspace::userspace_manager().write_file(pid, static_cast<uint32_t>(arg0), buffer, arg2);
        if (bytes != 0) return {bytes, kErrorNone};
        if (hk::userspace::userspace_manager().pipe_write_would_block(pid, static_cast<uint32_t>(arg0))) return {0, kErrorWouldBlock};
        return {0, kErrorNotFound};
    }
    case Number::DeleteFile: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().delete_file(pid, path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::CreateDirectory: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().create_directory(pid, path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Link: {
        auto* existing = reinterpret_cast<const char*>(arg0);
        auto* linked = reinterpret_cast<const char*>(arg2);
        if (!c_string_readable(existing, arg1) || !c_string_readable(linked, arg3)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().link_file(pid, existing, linked) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Truncate: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().truncate_file(pid, path, arg2) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Rename: {
        auto* old_path = reinterpret_cast<const char*>(arg0);
        auto* new_path = reinterpret_cast<const char*>(arg2);
        if (!c_string_readable(old_path, arg1) || !c_string_readable(new_path, arg3)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().rename_path(pid, old_path, new_path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::DeleteDirectory: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().delete_directory(pid, path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::RedirectProcessFd: {
        auto* path = reinterpret_cast<const char*>(arg2);
        if (!c_string_readable(path, arg3)) return {0, kErrorInvalidPointer};
        uint64_t caller = syscall_current_pid();
        auto* process = hk::userspace::userspace_manager().find_process(arg0);
        if (caller == 0 || !process || process->parent_pid != caller) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().redirect_file(arg0, static_cast<uint32_t>(arg1), path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::RedirectProcessFdAppend: {
        auto* path = reinterpret_cast<const char*>(arg2);
        if (!c_string_readable(path, arg3)) return {0, kErrorInvalidPointer};
        uint64_t caller = syscall_current_pid();
        auto* process = hk::userspace::userspace_manager().find_process(arg0);
        if (caller == 0 || !process || process->parent_pid != caller) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().redirect_file_append(arg0, static_cast<uint32_t>(arg1), path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::CreatePipe: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        uint32_t pipe_id = hk::userspace::userspace_manager().create_pipe();
        return pipe_id == 0 ? Result{0, kErrorNotFound} : Result{pipe_id, kErrorNone};
    }
    case Number::AttachPipeFd: {
        uint64_t caller = syscall_current_pid();
        auto* process = hk::userspace::userspace_manager().find_process(arg0);
        if (caller == 0 || !process || process->parent_pid != caller) return {0, kErrorNotFound};
        bool write_end = static_cast<hybrid::PipeEndpoint>(arg3) == hybrid::PipeEndpoint::Write;
        return hk::userspace::userspace_manager().attach_pipe_fd(arg0, static_cast<uint32_t>(arg1), static_cast<uint32_t>(arg2), write_end)
            ? Result{1, kErrorNone}
            : Result{0, kErrorNotFound};
    }
    case Number::ClosePipe: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().close_pipe(static_cast<uint32_t>(arg0)) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Spawn: {
        auto* command_line = reinterpret_cast<const char*>(arg0);
        char command[kMaxSpawnCommandLine];
        if (!copy_spawn_command(command_line, arg1, command)) return {0, kErrorInvalidPointer};
        const char* args[hk::userspace::kMaxProcessArguments]{};
        uint32_t arg_count = parse_spawn_arguments(command, args, hk::userspace::kMaxProcessArguments);
        if (arg_count == 0) return {0, kErrorNotFound};
        const char* path = args[0];
        auto* out_pid = reinterpret_cast<uint64_t*>(arg2);
        if (out_pid && !user_buffer_writable(reinterpret_cast<char*>(out_pid), sizeof(*out_pid))) return {0, kErrorInvalidPointer};
        const auto* node = hk::fs::vfs().find(path);
        if (!node || node->type != hk::fs::NodeType::MemoryFile) return {0, kErrorNotFound};
        auto* process = hk::userspace::userspace_manager().create_process_from_elf(path, node->base, node->size);
        if (!process) return {0, kErrorNotFound};
        uint64_t pid = process->pid;
        uint64_t parent_pid = syscall_current_pid();
        if (parent_pid != 0) {
            hk::userspace::userspace_manager().set_parent(pid, parent_pid);
            hybrid::PathInfo parent_cwd{};
            if (hk::userspace::userspace_manager().copy_current_directory(parent_pid, parent_cwd)) {
                hk::userspace::userspace_manager().set_current_directory(pid, parent_cwd.path);
            }
            hk::userspace::userspace_manager().inherit_environment(pid, parent_pid);
            hk::userspace::userspace_manager().inherit_standard_fds(pid, parent_pid);
        } else {
            static const char* env_keys[] = {"ROOT", "PATH"};
            static const char* env_values[] = {"/", "/bin"};
            hk::userspace::userspace_manager().set_environment(pid, env_keys, env_values, 2);
        }
        if (!hk::userspace::userspace_manager().set_arguments(pid, args, arg_count)) return {0, kErrorNotFound};
        if ((arg3 & hybrid::SpawnFlagStartSuspended) == 0) {
            if (!hk::userspace::userspace_manager().mark_runnable(pid)) return {0, kErrorNotFound};
            if (!auto_enable_user_preemption_if_competing()) return {0, kErrorNotFound};
        }
        if (out_pid) *out_pid = pid;
        return Result{pid, kErrorNone};
    }
    case Number::StartProcess: {
        uint64_t caller = syscall_current_pid();
        auto* process = hk::userspace::userspace_manager().find_process(arg0);
        if (caller == 0 || !process || process->parent_pid != caller) return {0, kErrorNotFound};
        if (!hk::userspace::userspace_manager().mark_runnable(arg0)) return {0, kErrorNotFound};
        return auto_enable_user_preemption_if_competing() ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Exit: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().exit_process(pid, arg0) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Kill: {
        uint64_t caller = syscall_current_pid();
        auto* process = hk::userspace::userspace_manager().find_process(arg0);
        if (caller == 0 || !process || process->pid == caller) return {0, kErrorNotFound};
        auto* caller_process = hk::userspace::userspace_manager().find_process(caller);
        bool direct_child = process->parent_pid == caller;
        bool same_parent = caller_process && caller_process->parent_pid != 0 && process->parent_pid == caller_process->parent_pid;
        if (!direct_child && !same_parent) return {0, kErrorNotFound};
        hybrid::ProcessTerminationReason reason = hybrid::ProcessTerminationReason::SigTerm;
        uint64_t exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
        if (arg1 == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill)) {
            reason = hybrid::ProcessTerminationReason::SigKill;
            exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill);
        } else if (arg1 == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm) || arg1 == 0) {
            reason = hybrid::ProcessTerminationReason::SigTerm;
            exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
        } else {
            return {0, kErrorNotFound};
        }
        return hk::userspace::userspace_manager().exit_process(process->pid, exit_code, reason) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::SetProcessGroup: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0 || arg0 == 0 || arg1 == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().set_process_group(caller, arg0, arg1) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::KillProcessGroup: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0 || arg0 == 0) return {0, kErrorNotFound};
        hybrid::ProcessTerminationReason reason = hybrid::ProcessTerminationReason::SigTerm;
        uint64_t exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
        if (arg1 == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill)) {
            reason = hybrid::ProcessTerminationReason::SigKill;
            exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill);
        } else if (arg1 == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm) || arg1 == 0) {
            reason = hybrid::ProcessTerminationReason::SigTerm;
            exit_code = 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
        } else {
            return {0, kErrorNotFound};
        }
        uint64_t killed = hk::userspace::userspace_manager().kill_process_group(caller, arg0, exit_code, reason);
        return killed != 0 ? Result{killed, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::SetForegroundProcessGroup: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().set_foreground_process_group(caller, arg0) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetForegroundProcessGroup:
        return {hk::userspace::userspace_manager().foreground_process_group(), kErrorNone};
    case Number::StopProcessGroup: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0 || arg0 == 0) return {0, kErrorNotFound};
        uint64_t stopped = hk::userspace::userspace_manager().stop_process_group(caller, arg0);
        return stopped != 0 ? Result{stopped, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::ContinueProcessGroup: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0 || arg0 == 0) return {0, kErrorNotFound};
        uint64_t continued = hk::userspace::userspace_manager().continue_process_group(caller, arg0);
        if (continued != 0 && !auto_enable_user_preemption_if_competing()) return {0, kErrorNotFound};
        return continued != 0 ? Result{continued, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::Wait: {
        auto* exit_code = reinterpret_cast<uint64_t*>(arg1);
        if (!exit_code || !user_buffer_writable(reinterpret_cast<char*>(exit_code), sizeof(*exit_code))) return {0, kErrorInvalidPointer};
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        uint64_t code = 0;
        if (!hk::userspace::userspace_manager().wait_process(caller, arg0, code)) {
            if (hk::userspace::userspace_manager().process_wait_would_block(caller, arg0)) return {0, kErrorWouldBlock};
            return {0, kErrorNotFound};
        }
        *exit_code = code;
        return {1, kErrorNone};
    }
    case Number::WaitAny: {
        auto* out = reinterpret_cast<hybrid::WaitAnyInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        hybrid::WaitAnyInfo info{};
        if (!hk::userspace::userspace_manager().wait_any_process(caller, info)) {
            if (hk::userspace::userspace_manager().process_wait_any_would_block(caller)) return {0, kErrorWouldBlock};
            return {0, kErrorNotFound};
        }
        *out = info;
        return {1, kErrorNone};
    }
    case Number::GetUserSchedulerInfo: {
        auto* out = reinterpret_cast<hybrid::UserSchedulerInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_user_scheduler_info(*out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetCurrentUserContext: {
        auto* out = reinterpret_cast<hybrid::CurrentUserContextInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_current_user_context(*out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetCurrentIds: {
        auto* out = reinterpret_cast<hybrid::CurrentIdsInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        uint64_t tid = syscall_current_tid();
        if (pid == 0 || tid == 0) return {0, kErrorNotFound};
        auto* process = hk::userspace::userspace_manager().find_process(pid);
        auto* kernel_thread = hk::sched::scheduler().current_thread();
        out->pid = pid;
        out->tid = tid;
        out->parent_pid = process ? process->parent_pid : 0;
        out->kernel_thread_id = kernel_thread ? kernel_thread->id : 0;
        out->process_group_id = process ? process->process_group_id : 0;
        out->cpu_id = hk::cpu::runtime().current_cpu_id();
        out->reserved = 0;
        return {1, kErrorNone};
    }
    case Number::SetUserPreemption: {
        uint64_t caller = syscall_current_pid();
        if (caller == 0) return {0, kErrorNotFound};
        bool enabled = arg0 != 0;
        if (enabled) {
            if (!hk::timer::lapic_timer_active() && !hk::timer::start_lapic_system_tick(0x400000)) return {0, kErrorNotFound};
            hk::timer::set_user_preemption_enabled(true);
            hk::log(hk::LogLevel::Info, "Userspace Local APIC preemption enabled");
            return {1, kErrorNone};
        }
        if (!hk::userspace::userspace_manager().update_user_preemption_gate()) return {0, kErrorNotFound};
        if (hk::timer::user_preemption_enabled()) {
            hk::log(hk::LogLevel::Info, "Userspace auto preemption retained");
            return {1, kErrorNone};
        }
        hk::log(hk::LogLevel::Info, "Userspace Local APIC preemption disabled");
        return {0, kErrorNone};
    }
    case Number::SelectNextUserThread: {
        auto* out = reinterpret_cast<hybrid::LaunchContextInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        hk::userspace::UserLaunchContext context{};
        if (!hk::userspace::userspace_manager().select_next_runnable_thread(context)) return {0, kErrorNotFound};
        out->tid = context.tid;
        out->pid = context.pid;
        out->rip = context.rip;
        out->rsp = context.rsp;
        out->cr3 = context.cr3;
        out->cs = context.cs;
        out->ss = context.ss;
        out->reserved = 0;
        out->rflags = context.rflags;
        return {context.tid, kErrorNone};
    }
    case Number::GetDeviceCount:
        return {hk::drivers::inventory().count(), kErrorNone};
    case Number::GetStorageDeviceCount:
        return {hk::drivers::inventory().storage_count(), kErrorNone};
    case Number::GetNetworkDeviceCount:
        return {hk::drivers::inventory().network_count(), kErrorNone};
    case Number::GetDisplayDeviceCount:
        return {hk::drivers::inventory().display_count(), kErrorNone};
    case Number::GetDeviceInfo: {
        auto* out = reinterpret_cast<hybrid::DeviceInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::drivers::inventory().copy_info(static_cast<uint32_t>(arg0), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetDeviceInfoByClass: {
        auto* out = reinterpret_cast<hybrid::DeviceInfo*>(arg2);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::drivers::inventory().copy_info_by_class(static_cast<hybrid::DeviceClass>(arg0), static_cast<uint32_t>(arg1), *out)
            ? Result{1, kErrorNone}
            : Result{0, kErrorNotFound};
    }
    case Number::GetFramebufferInfo: {
        auto* out = reinterpret_cast<hybrid::FramebufferInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        const auto& fb = hk::boot::framebuffer_info();
        if (fb.base == 0 || fb.width == 0 || fb.height == 0) return {0, kErrorNotFound};
        *out = fb;
        return {1, kErrorNone};
    }
    case Number::GetMemoryStats: {
        auto* out = reinterpret_cast<hybrid::MemoryStatsInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        const auto stats = hk::mm::pmm().stats();
        out->total_pages = stats.total_pages;
        out->free_pages = stats.free_pages;
        out->used_pages = stats.used_pages;
        out->usable_bytes = stats.usable_bytes;
        out->reserved_bytes = stats.reserved_bytes;
        out->highest_physical = stats.highest_physical;
        return {1, kErrorNone};
    }
    case Number::GetBlockDeviceInfo: {
        auto* out = reinterpret_cast<hybrid::BlockDeviceInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        const auto stats = hk::block::boot_disk().stats();
        out->sector_size = 512;
        out->sector_count = stats.sector_count;
        out->sector_reads = stats.sector_reads;
        out->cache_hits = stats.cache_hits;
        out->cache_misses = stats.cache_misses;
        out->cache_evictions = stats.cache_evictions;
        out->invalid_reads = stats.invalid_reads;
        out->null_buffer_rejects = stats.null_buffer_rejects;
        out->zero_count_rejects = stats.zero_count_rejects;
        out->oversized_request_rejects = stats.oversized_request_rejects;
        out->backend_read_failures = stats.backend_read_failures;
        out->cache_fills = stats.cache_fills;
        out->cached_entries = stats.cached_entries;
        out->largest_request_sectors = stats.largest_request_sectors;
        out->last_lba = stats.last_lba;
        out->initialized = stats.initialized ? 1u : 0u;
        out->reserved = 0;
        return {1, kErrorNone};
    }
    case Number::ReadBlockSector: {
        auto* out = reinterpret_cast<unsigned char*>(arg1);
        if (!user_buffer_writable(reinterpret_cast<char*>(out), 512)) return {0, kErrorInvalidPointer};
        unsigned char sector[512]{};
        if (!hk::block::boot_disk().read_sector(arg0, sector)) return {0, kErrorNotFound};
        memcpy(out, sector, sizeof(sector));
        return {512, kErrorNone};
    }
    case Number::GetSystemInfo: {
        auto* out = reinterpret_cast<hybrid::SystemInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        const auto& boot = hk::boot::retained_boot_info();
        out->boot_info_version = boot.version;
        out->boot_info_flags = boot.flags;
        out->boot_module_count = boot.boot_module_count;
        out->kernel_base = boot.kernel_physical_base;
        out->kernel_end = boot.kernel_physical_end;
        out->kernel_entry = boot.kernel_entry;
        out->rsdp = boot.rsdp;
        copy_text(out->sysname, sizeof(out->sysname), hybrid::version::kOsName);
        copy_text(out->release, sizeof(out->release), hybrid::version::kOsRelease);
        copy_text(out->machine, sizeof(out->machine), hybrid::version::kMachine);
        if ((boot.flags & hybrid::kBootFlagDebug) != 0) copy_text(out->boot_mode, sizeof(out->boot_mode), "uefi-debug");
        else if ((boot.flags & hybrid::kBootFlagRecovery) != 0) copy_text(out->boot_mode, sizeof(out->boot_mode), "uefi-recovery");
        else copy_text(out->boot_mode, sizeof(out->boot_mode), "uefi");
        copy_text(out->kernel_type, sizeof(out->kernel_type), hybrid::version::kKernelDisplay);
        return {1, kErrorNone};
    }
    case Number::GetLimitsInfo: {
        auto* out = reinterpret_cast<hybrid::LimitsInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        out->max_boot_modules = hybrid::kMaxBootModules;
        out->max_vfs_nodes = hk::fs::kMaxVfsNodes;
        out->max_file_handles = hk::fs::kMaxFileHandles;
        out->max_ram_files = hk::fs::kMaxRamFiles;
        out->max_ram_directories = hk::fs::kMaxRamDirectories;
        out->max_ram_links = hk::fs::kMaxRamLinks;
        out->max_mounts = hk::fs::kMaxMounts;
        out->max_ram_file_bytes = hk::fs::kMaxRamFileBytes;
        out->max_process_file_descriptors = hk::userspace::kMaxProcessFileDescriptors;
        out->max_owned_user_pages = hk::userspace::kMaxOwnedUserPages;
        out->max_process_arguments = hk::userspace::kMaxProcessArguments;
        out->max_argument_length = hk::userspace::kMaxArgumentLength;
        out->max_environment_entries = hk::userspace::kMaxEnvironmentEntries;
        out->max_environment_key_length = hk::userspace::kMaxEnvironmentKeyLength;
        out->max_environment_value_length = hk::userspace::kMaxEnvironmentValueLength;
        out->max_pipes = hk::userspace::kMaxPipes;
        out->pipe_capacity = hk::userspace::kPipeCapacity;
        out->max_cpus = hk::cpu::kMaxCpus;
        out->pmm_bitmap_pages = hk::mm::kPmmBitmapPages;
        out->mounted_fat_path_capacity = hk::fs::mounted_fat_path_capacity();
        return {1, kErrorNone};
    }
    case Number::GetAbiInfo: {
        auto* out = reinterpret_cast<hybrid::AbiInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        out->abi_version = hybrid::kSyscallAbiVersion;
        out->boot_info_version = hybrid::kBootInfoVersion;
        out->syscall_max_number = hybrid::kSyscallMaxNumber;
        out->syscall_result_size = sizeof(hybrid::SyscallResult);
        out->boot_info_size = sizeof(hybrid::BootInfo);
        out->framebuffer_info_size = sizeof(hybrid::FramebufferInfo);
        out->memory_region_size = sizeof(hybrid::MemoryRegion);
        out->boot_module_size = sizeof(hybrid::BootModule);
        out->system_info_size = sizeof(hybrid::SystemInfo);
        out->limits_info_size = sizeof(hybrid::LimitsInfo);
        out->abi_info_size = sizeof(hybrid::AbiInfo);
        out->process_info_size = sizeof(hybrid::ProcessInfo);
        out->user_thread_info_size = sizeof(hybrid::UserThreadInfo);
        out->vfs_node_info_size = sizeof(hybrid::VfsNodeInfo);
        out->vfs_stat_info_size = sizeof(hybrid::VfsStatInfo);
        out->mount_info_size = sizeof(hybrid::MountInfo);
        out->file_descriptor_info_size = sizeof(hybrid::FileDescriptorInfo);
        out->pipe_info_size = sizeof(hybrid::PipeInfo);
        out->block_device_info_size = sizeof(hybrid::BlockDeviceInfo);
        out->feature_info_size = sizeof(hybrid::FeatureInfo);
        return {1, kErrorNone};
    }
    case Number::GetFeatureInfo: {
        auto* out = reinterpret_cast<hybrid::FeatureInfo*>(arg0);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        out->flags =
            hybrid::KernelFeatureUefiBoot |
            hybrid::KernelFeatureFramebufferConsole |
            hybrid::KernelFeatureSerialLog |
            hybrid::KernelFeatureGdt |
            hybrid::KernelFeatureIdt |
            hybrid::KernelFeatureSyscalls |
            hybrid::KernelFeaturePmmBitmap |
            hybrid::KernelFeatureVmmPageTables |
            hybrid::KernelFeatureKernelHeap |
            hybrid::KernelFeatureVfs |
            hybrid::KernelFeatureRamFs |
            hybrid::KernelFeatureProcFs |
            hybrid::KernelFeatureDevFs |
            hybrid::KernelFeatureFat16Mount |
            hybrid::KernelFeatureElfUserspace |
            hybrid::KernelFeatureScheduler |
            hybrid::KernelFeaturePreemption |
            hybrid::KernelFeaturePipes |
            hybrid::KernelFeatureJobControl |
            hybrid::KernelFeatureSmp |
            hybrid::KernelFeatureLocalApic |
            hybrid::KernelFeatureIoApic |
            hybrid::KernelFeaturePci |
            hybrid::KernelFeatureAhci |
            hybrid::KernelFeatureE1000 |
            hybrid::KernelFeaturePs2Keyboard |
            hybrid::KernelFeatureTtyScrollback |
            hybrid::KernelFeatureRecoveryMode |
            hybrid::KernelFeatureDebugBoot |
            hybrid::KernelFeatureBlockCache;
        out->experimental_flags = 0;
        out->stable_count = 30;
        out->experimental_count = 0;
        return {1, kErrorNone};
    }
    case Number::ReadKernelLog: {
        auto* out = reinterpret_cast<char*>(arg0);
        if (!user_buffer_writable(out, arg1)) return {0, kErrorInvalidPointer};
        return {hk::copy_kernel_log(out, arg1, arg2), kErrorNone};
    }
    case Number::GetProcessInfo: {
        auto* out = reinterpret_cast<hybrid::ProcessInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_process_info(arg0, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetUserThreadInfo: {
        auto* out = reinterpret_cast<hybrid::UserThreadInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_thread_info(arg0, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetFileDescriptorInfo: {
        auto* out = reinterpret_cast<hybrid::FileDescriptorInfo*>(arg2);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        uint64_t caller = syscall_current_pid();
        uint64_t target_pid = arg0 == 0 ? caller : arg0;
        auto* process = hk::userspace::userspace_manager().find_process(target_pid);
        auto* caller_process = hk::userspace::userspace_manager().find_process(caller);
        bool direct_child = process && process->parent_pid == caller;
        bool same_parent = caller_process && caller_process->parent_pid != 0 && process && process->parent_pid == caller_process->parent_pid;
        if (caller == 0 || !process || (target_pid != caller && !direct_child && !same_parent)) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().copy_file_descriptor_info(target_pid, arg1, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetPipeCount:
        return {hk::userspace::userspace_manager().pipe_count(), kErrorNone};
    case Number::GetPipeInfo: {
        auto* out = reinterpret_cast<hybrid::PipeInfo*>(arg1);
        if (!out || !user_buffer_writable(reinterpret_cast<char*>(out), sizeof(*out))) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_pipe_info(arg0, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetSchedulerStats: {
        auto* out = reinterpret_cast<hybrid::SchedulerStatsInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        auto& sched = hk::sched::scheduler();
        auto* current = sched.current_thread();
        out->thread_count = sched.thread_count();
        out->ready_count = sched.ready_count();
        out->sleeping_count = sched.sleeping_count();
        out->dead_count = sched.dead_count();
        out->switch_count = sched.switch_count();
        out->yield_count = sched.yield_count();
        out->preempt_count = sched.preempt_count();
        out->current_thread_id = current ? current->id : 0;
        out->current_cpu_id = hk::cpu::runtime().current_cpu_id();
        out->online_cpu_count = hk::cpu::topology().online_count();
        return {1, kErrorNone};
    }
    case Number::GetCpuInfo: {
        auto* out = reinterpret_cast<hybrid::CpuInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        if (!hk::cpu::topology().copy_info(static_cast<uint32_t>(arg0), *out)) return {0, kErrorNotFound};
        hk::cpu::runtime().decorate_cpu_info(static_cast<uint32_t>(arg0), *out);
        return {1, kErrorNone};
    }
    case Number::GetLaunchContext: {
        auto* out = reinterpret_cast<hybrid::LaunchContextInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::userspace::userspace_manager().copy_launch_context(arg0, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetVfsNodeCount:
        return {hk::fs::vfs().node_count(), kErrorNone};
    case Number::GetVfsNodeInfo: {
        auto* out = reinterpret_cast<hybrid::VfsNodeInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::fs::vfs().copy_node_info(static_cast<uint32_t>(arg0), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::ReadDirectory: {
        auto* path = reinterpret_cast<const char*>(arg0);
        auto* out = reinterpret_cast<hybrid::VfsDirectoryEntryInfo*>(arg3);
        if (!c_string_readable(path, arg1) || !out) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid != 0) {
            return hk::userspace::userspace_manager().copy_directory_entry(pid, path, static_cast<uint32_t>(arg2), *out)
                ? Result{1, kErrorNone}
                : Result{0, kErrorNotFound};
        }
        return hk::fs::vfs().copy_directory_entry(path, static_cast<uint32_t>(arg2), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::ReadLink: {
        auto* path = reinterpret_cast<const char*>(arg0);
        auto* out = reinterpret_cast<char*>(arg2);
        if (!c_string_readable(path, arg1) || !user_buffer_writable(out, arg3)) return {0, kErrorInvalidPointer};
        constexpr const char* kProcSelfFdPrefix = "/proc/self/fd/";
        hybrid::FileDescriptorInfo info{};
        if (starts_with(path, kProcSelfFdPrefix)) {
            uint32_t fd = 0;
            if (!parse_decimal_tail(path + 14, fd) || !proc_self_fd_target(fd, info)) return {0, kErrorNotFound};
            return link_target_result(out, arg3, info);
        }
        uint64_t pid = 0;
        uint32_t fd = 0;
        if (!parse_proc_pid_fd_path(path, pid, fd) || !proc_fd_target(pid, fd, info)) return {0, kErrorNotFound};
        return link_target_result(out, arg3, info);
    }
    case Number::GetMountCount:
        return {hk::fs::vfs().mount_count(), kErrorNone};
    case Number::GetMountInfo: {
        auto* out = reinterpret_cast<hybrid::MountInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        return hk::fs::vfs().copy_mount_info(static_cast<uint32_t>(arg0), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetCurrentProcessId:
        return {syscall_current_pid(), kErrorNone};
    case Number::GetCurrentDirectory: {
        auto* out = reinterpret_cast<hybrid::PathInfo*>(arg0);
        if (!out) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().copy_current_directory(pid, *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::SetCurrentDirectory: {
        auto* path = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(path, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().set_current_directory(pid, path) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetArgumentCount: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return {hk::userspace::userspace_manager().argument_count(pid), kErrorNone};
    }
    case Number::GetArgument: {
        auto* out = reinterpret_cast<hybrid::ArgumentInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().copy_argument(pid, static_cast<uint32_t>(arg0), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::GetEnvironmentCount: {
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return {hk::userspace::userspace_manager().environment_count(pid), kErrorNone};
    }
    case Number::GetEnvironment: {
        auto* out = reinterpret_cast<hybrid::EnvironmentInfo*>(arg1);
        if (!out) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().copy_environment(pid, static_cast<uint32_t>(arg0), *out) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::SetEnvironment: {
        auto* key = reinterpret_cast<const char*>(arg0);
        auto* value = reinterpret_cast<const char*>(arg2);
        if (!c_string_readable(key, arg1) || !c_string_readable(value, arg3)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().set_environment_variable(pid, key, value) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::UnsetEnvironment: {
        auto* key = reinterpret_cast<const char*>(arg0);
        if (!c_string_readable(key, arg1)) return {0, kErrorInvalidPointer};
        uint64_t pid = syscall_current_pid();
        if (pid == 0) return {0, kErrorNotFound};
        return hk::userspace::userspace_manager().unset_environment_variable(pid, key) ? Result{1, kErrorNone} : Result{0, kErrorNotFound};
    }
    case Number::ReadKey: {
        char c = 0;
        if (hk::terminal::read_key(&c, 1) != 1) return {0, kErrorNotFound};
        return {static_cast<uint64_t>(static_cast<uint8_t>(c)), kErrorNone};
    }
    case Number::Write: {
        const char* data = nullptr;
        uint64_t length = 0;
        if ((arg0 == hybrid::kStdoutFd || arg0 == hybrid::kStderrFd) && arg2 != 0) {
            data = reinterpret_cast<const char*>(arg1);
            length = arg2;
        } else if (arg2 == 0) {
            data = reinterpret_cast<const char*>(arg0);
            length = arg1;
        } else {
            return {0, kErrorNotFound};
        }
        if (!user_buffer_readable(data, length)) return {0, kErrorInvalidPointer};
        if (arg0 == hybrid::kStdoutFd || arg0 == hybrid::kStderrFd) {
            uint64_t pid = syscall_current_pid();
            if (pid != 0) {
                uint64_t bytes = hk::userspace::userspace_manager().write_file(pid, static_cast<uint32_t>(arg0), data, length);
                if (bytes != 0) return {bytes, kErrorNone};
                if (hk::userspace::userspace_manager().pipe_write_would_block(pid, static_cast<uint32_t>(arg0))) return {0, kErrorWouldBlock};
                if (hk::userspace::userspace_manager().is_pipe_fd(pid, static_cast<uint32_t>(arg0))) return {0, kErrorNotFound};
            }
        }
        return {hk::terminal::write(data, static_cast<size_t>(length)), kErrorNone};
    }
    case Number::TerminalControl: {
        switch (static_cast<hybrid::TerminalControlCommand>(arg0)) {
        case hybrid::TerminalControlCommand::ScrollRelative:
            hk::terminal::scroll_relative(static_cast<int64_t>(arg1));
            return {1, kErrorNone};
        case hybrid::TerminalControlCommand::ScrollToBottom:
            hk::terminal::scroll_to_bottom();
            return {1, kErrorNone};
        case hybrid::TerminalControlCommand::SetInputMode:
            hk::terminal::set_input_mode(static_cast<hybrid::TerminalInputMode>(arg1));
            return {1, kErrorNone};
        case hybrid::TerminalControlCommand::GetInputMode:
            return {static_cast<uint64_t>(hk::terminal::input_mode()), kErrorNone};
        case hybrid::TerminalControlCommand::InjectInput: {
            auto* data = reinterpret_cast<const char*>(arg1);
            if (!user_buffer_readable(data, arg2)) return {0, kErrorInvalidPointer};
            return {hk::terminal::inject_input(data, static_cast<size_t>(arg2)), kErrorNone};
        }
        case hybrid::TerminalControlCommand::ResetInputLine:
            hk::terminal::reset_input_line();
            return {1, kErrorNone};
        default:
            return {0, kErrorNotFound};
        }
    }
    default:
        return {0, kErrorInvalidSyscall};
    }
}

bool self_test() {
    auto invalid = dispatch(0xffff, 0, 0, 0, 0);
    if (invalid.error != kErrorInvalidSyscall) {
        hk::log(hk::LogLevel::Error, "syscall self-test invalid-number FAIL");
        return false;
    }
    auto ticks = dispatch(static_cast<uint64_t>(Number::GetTicks), 0, 0, 0, 0);
    if (ticks.error != kErrorNone) {
        hk::log(hk::LogLevel::Error, "syscall self-test get-ticks FAIL");
        return false;
    }
    hybrid::DateTimeInfo datetime{};
    auto datetime_result = dispatch(static_cast<uint64_t>(Number::GetDateTime), reinterpret_cast<uint64_t>(&datetime), 0, 0, 0);
    if (datetime_result.error != kErrorNone || datetime_result.value != 1 ||
        datetime.year < 2020 || datetime.year >= 2100 ||
        datetime.month < 1 || datetime.month > 12 ||
        datetime.day < 1 || datetime.day > 31 ||
        datetime.hour >= 24 || datetime.minute >= 60 || datetime.second >= 60) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test datetime error", datetime_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test datetime value", datetime_result.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall datetime self-test");
    auto thread_id = dispatch(static_cast<uint64_t>(Number::GetThreadId), 0, 0, 0, 0);
    if (thread_id.error != kErrorNone || thread_id.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-id error", thread_id.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-id value", thread_id.value);
        return false;
    }
    hybrid::CurrentIdsInfo current_ids{};
    auto ids_result = dispatch(static_cast<uint64_t>(Number::GetCurrentIds), reinterpret_cast<uint64_t>(&current_ids), 0, 0, 0);
    if (ids_result.error != kErrorNone || ids_result.value != 1 ||
        current_ids.pid == 0 || current_ids.tid == 0 || current_ids.kernel_thread_id == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test current-ids error", ids_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test current-ids value", ids_result.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall current ids self-test");
    auto pipe_before = dispatch(static_cast<uint64_t>(Number::GetPipeCount), 0, 0, 0, 0);
    auto pipe = dispatch(static_cast<uint64_t>(Number::CreatePipe), 0, 0, 0, 0);
    auto* pipe_child = hk::userspace::userspace_manager().create_process_stub("pipe-info-selftest", 0x405000, 0x5000);
    if (!pipe_child) return false;
    uint64_t pipe_child_pid = pipe_child->pid;
    if (!hk::userspace::userspace_manager().set_parent(pipe_child_pid, current_ids.pid)) return false;
    auto attach_read = dispatch(static_cast<uint64_t>(Number::AttachPipeFd), pipe_child_pid, hybrid::kStdinFd, pipe.value, static_cast<uint64_t>(hybrid::PipeEndpoint::Read));
    auto attach_write = dispatch(static_cast<uint64_t>(Number::AttachPipeFd), pipe_child_pid, hybrid::kStdoutFd, pipe.value, static_cast<uint64_t>(hybrid::PipeEndpoint::Write));
    auto pipe_after = dispatch(static_cast<uint64_t>(Number::GetPipeCount), 0, 0, 0, 0);
    bool saw_pipe = false;
    hybrid::PipeInfo pipe_info{};
    for (uint64_t i = 0; i < pipe_after.value; ++i) {
        auto info = dispatch(static_cast<uint64_t>(Number::GetPipeInfo), i, reinterpret_cast<uint64_t>(&pipe_info), 0, 0);
        if (info.error == kErrorNone && pipe_info.id == pipe.value) {
            saw_pipe = true;
            break;
        }
    }
    bool closed_read = hk::userspace::userspace_manager().close_file(pipe_child_pid, hybrid::kStdinFd);
    bool closed_write = hk::userspace::userspace_manager().close_file(pipe_child_pid, hybrid::kStdoutFd);
    bool pipe_child_exited = hk::userspace::userspace_manager().exit_process(pipe_child_pid, 0);
    bool pipe_child_reaped = hk::userspace::userspace_manager().reap_exited(pipe_child_pid);
    auto pipe_final = dispatch(static_cast<uint64_t>(Number::GetPipeCount), 0, 0, 0, 0);
    if (pipe_before.error != kErrorNone ||
        pipe.error != kErrorNone || pipe.value == 0 ||
        attach_read.error != kErrorNone || attach_write.error != kErrorNone ||
        pipe_after.error != kErrorNone || pipe_after.value != pipe_before.value + 1 ||
        !saw_pipe || pipe_info.open != 1 || pipe_info.capacity != hk::userspace::kPipeCapacity ||
        pipe_info.reader_count != 1 || pipe_info.writer_count != 1 ||
        !closed_read || !closed_write || !pipe_child_exited || !pipe_child_reaped ||
        pipe_final.error != kErrorNone || pipe_final.value != pipe_before.value) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test pipe-info before", pipe_before.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test pipe-info pipe", pipe.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test pipe-info after", pipe_after.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test pipe-info readers", pipe_info.reader_count);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test pipe-info writers", pipe_info.writer_count);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall pipe info self-test");
    auto* group_child = hk::userspace::userspace_manager().create_process_stub("pgrp-selftest", 0x406000, 0x6000);
    if (!group_child) return false;
    uint64_t group_child_pid = group_child->pid;
    if (!hk::userspace::userspace_manager().set_parent(group_child_pid, current_ids.pid)) return false;
    auto set_group = dispatch(static_cast<uint64_t>(Number::SetProcessGroup), group_child_pid, group_child_pid, 0, 0);
    auto kill_group = dispatch(static_cast<uint64_t>(Number::KillProcessGroup), group_child_pid, static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm), 0, 0);
    uint64_t group_exit = 0;
    bool group_waited = hk::userspace::userspace_manager().wait_process(current_ids.pid, group_child_pid, group_exit);
    bool group_reaped = hk::userspace::userspace_manager().reap_exited(group_child_pid);
    if (set_group.error != kErrorNone || kill_group.error != kErrorNone || kill_group.value != 1 ||
        !group_waited || group_exit != 128 + static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm) || !group_reaped) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-group set error", set_group.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-group kill error", kill_group.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-group kill value", kill_group.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall process group self-test");
    auto* foreground_child = hk::userspace::userspace_manager().create_process_stub("fgpgrp-selftest", 0x407000, 0x7000);
    if (!foreground_child) return false;
    uint64_t foreground_child_pid = foreground_child->pid;
    if (!hk::userspace::userspace_manager().set_parent(foreground_child_pid, current_ids.pid)) return false;
    auto set_foreground_group = dispatch(static_cast<uint64_t>(Number::SetProcessGroup), foreground_child_pid, foreground_child_pid, 0, 0);
    auto foreground_child_set = dispatch(static_cast<uint64_t>(Number::SetForegroundProcessGroup), foreground_child_pid, 0, 0, 0);
    auto foreground_child_get = dispatch(static_cast<uint64_t>(Number::GetForegroundProcessGroup), 0, 0, 0, 0);
    auto foreground_shell_set = dispatch(static_cast<uint64_t>(Number::SetForegroundProcessGroup), current_ids.process_group_id, 0, 0, 0);
    auto foreground_shell_get = dispatch(static_cast<uint64_t>(Number::GetForegroundProcessGroup), 0, 0, 0, 0);
    if (!hk::userspace::userspace_manager().exit_process(foreground_child_pid, 0) ||
        !hk::userspace::userspace_manager().reap_exited(foreground_child_pid) ||
        set_foreground_group.error != kErrorNone ||
        foreground_child_set.error != kErrorNone || foreground_child_get.value != foreground_child_pid ||
        foreground_shell_set.error != kErrorNone || foreground_shell_get.value != current_ids.process_group_id) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test foreground-group set error", foreground_child_set.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test foreground-group child", foreground_child_get.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test foreground-group shell", foreground_shell_get.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall foreground process group self-test");
    auto* stop_child = hk::userspace::userspace_manager().create_process_stub("stop-pgrp-selftest", 0x408000, 0x8000);
    if (!stop_child) return false;
    uint64_t stop_child_pid = stop_child->pid;
    if (!hk::userspace::userspace_manager().set_parent(stop_child_pid, current_ids.pid) ||
        !hk::userspace::userspace_manager().mark_runnable(stop_child_pid)) {
        return false;
    }
    auto stop_group_set = dispatch(static_cast<uint64_t>(Number::SetProcessGroup), stop_child_pid, stop_child_pid, 0, 0);
    uint64_t runnable_before_stop = hk::userspace::userspace_manager().runnable_count();
    auto stop_group = dispatch(static_cast<uint64_t>(Number::StopProcessGroup), stop_child_pid, 0, 0, 0);
    uint64_t runnable_after_stop = hk::userspace::userspace_manager().runnable_count();
    auto continue_group = dispatch(static_cast<uint64_t>(Number::ContinueProcessGroup), stop_child_pid, 0, 0, 0);
    uint64_t runnable_after_continue = hk::userspace::userspace_manager().runnable_count();
    if (!hk::userspace::userspace_manager().exit_process(stop_child_pid, 0) ||
        !hk::userspace::userspace_manager().reap_exited(stop_child_pid) ||
        stop_group_set.error != kErrorNone ||
        stop_group.error != kErrorNone || stop_group.value != 1 ||
        continue_group.error != kErrorNone || continue_group.value != 1 ||
        runnable_after_stop + 1 != runnable_before_stop ||
        runnable_after_continue != runnable_before_stop) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stop-group error", stop_group.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stop-group value", stop_group.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test continue-group error", continue_group.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test runnable after stop", runnable_after_stop);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall stop continue process group self-test");
    auto process_count = dispatch(static_cast<uint64_t>(Number::GetProcessCount), 0, 0, 0, 0);
    if (process_count.error != kErrorNone || process_count.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-count error", process_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-count value", process_count.value);
        return false;
    }
    auto thread_count = dispatch(static_cast<uint64_t>(Number::GetThreadCount), 0, 0, 0, 0);
    if (thread_count.error != kErrorNone || thread_count.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-count error", thread_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-count value", thread_count.value);
        return false;
    }
    auto runnable_count = dispatch(static_cast<uint64_t>(Number::GetRunnableProcessCount), 0, 0, 0, 0);
    if (runnable_count.error != kErrorNone || runnable_count.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test runnable-count error", runnable_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test runnable-count value", runnable_count.value);
        return false;
    }
    auto exited_count = dispatch(static_cast<uint64_t>(Number::GetExitedProcessCount), 0, 0, 0, 0);
    if (exited_count.error != kErrorNone) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test exited-count error", exited_count.error);
        return false;
    }
    auto user_thread_count = dispatch(static_cast<uint64_t>(Number::GetUserThreadCount), 0, 0, 0, 0);
    if (user_thread_count.error != kErrorNone || user_thread_count.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-thread-count error", user_thread_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-thread-count value", user_thread_count.value);
        return false;
    }
    auto runnable_user_threads = dispatch(static_cast<uint64_t>(Number::GetRunnableUserThreadCount), 0, 0, 0, 0);
    hybrid::UserSchedulerInfo initial_user_sched{};
    auto initial_user_sched_info = dispatch(static_cast<uint64_t>(Number::GetUserSchedulerInfo), reinterpret_cast<uint64_t>(&initial_user_sched), 0, 0, 0);
    if (runnable_user_threads.error != kErrorNone || initial_user_sched_info.error != kErrorNone ||
        (runnable_user_threads.value + initial_user_sched.running_threads) == 0 ||
        initial_user_sched.schedulable_threads == 0 || initial_user_sched.timeslice_quantum == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test runnable-user-threads error", runnable_user_threads.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test runnable-user-threads value", runnable_user_threads.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test running-user-threads value", initial_user_sched.running_threads);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched schedulable", initial_user_sched.schedulable_threads);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched quantum", initial_user_sched.timeslice_quantum);
        return false;
    }
    auto live_processes = dispatch(static_cast<uint64_t>(Number::GetLiveProcessCount), 0, 0, 0, 0);
    if (live_processes.error != kErrorNone || live_processes.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test live-processes error", live_processes.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test live-processes value", live_processes.value);
        return false;
    }
    auto reap_missing = dispatch(static_cast<uint64_t>(Number::ReapProcess), 0xffff, 0, 0, 0);
    if (reap_missing.error != kErrorNone || reap_missing.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test reap-missing error", reap_missing.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test reap-missing value", reap_missing.value);
        return false;
    }
    static const char init_path[] = "/user/init.elf";
    auto init_size = dispatch(static_cast<uint64_t>(Number::VfsStat), reinterpret_cast<uint64_t>(init_path), sizeof(init_path), 0, 0);
    if (init_size.error != kErrorNone || init_size.value < 4) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-stat error", init_size.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-stat value", init_size.value);
        return false;
    }
    unsigned char magic[4]{};
    auto init_read = dispatch(static_cast<uint64_t>(Number::VfsRead), reinterpret_cast<uint64_t>(init_path), sizeof(init_path), reinterpret_cast<uint64_t>(magic), sizeof(magic));
    if (init_read.error != kErrorNone || init_read.value != sizeof(magic) ||
        magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-read error", init_read.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-read value", init_read.value);
        return false;
    }
    auto opened = dispatch(static_cast<uint64_t>(Number::VfsOpen), reinterpret_cast<uint64_t>(init_path), sizeof(init_path), 0, 0);
    if (opened.error != kErrorNone || opened.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-open error", opened.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-open value", opened.value);
        return false;
    }
    unsigned char handle_magic[4]{};
    auto handle_read = dispatch(static_cast<uint64_t>(Number::VfsReadHandle), opened.value, reinterpret_cast<uint64_t>(handle_magic), sizeof(handle_magic), 0);
    if (handle_read.error != kErrorNone || handle_read.value != sizeof(handle_magic) ||
        handle_magic[0] != 0x7f || handle_magic[1] != 'E' || handle_magic[2] != 'L' || handle_magic[3] != 'F') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-read-handle error", handle_read.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-read-handle value", handle_read.value);
        return false;
    }
    auto closed = dispatch(static_cast<uint64_t>(Number::VfsClose), opened.value, 0, 0, 0);
    if (closed.error != kErrorNone || closed.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-close error", closed.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-close value", closed.value);
        return false;
    }
    auto fd_open = dispatch(static_cast<uint64_t>(Number::Open), reinterpret_cast<uint64_t>(init_path), sizeof(init_path), 0, 0);
    if (fd_open.error != kErrorNone || fd_open.value < 3) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test open error", fd_open.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test open value", fd_open.value);
        return false;
    }
    unsigned char fd_magic[4]{};
    auto fd_read = dispatch(static_cast<uint64_t>(Number::Read), fd_open.value, reinterpret_cast<uint64_t>(fd_magic), sizeof(fd_magic), 0);
    if (fd_read.error != kErrorNone || fd_read.value != sizeof(fd_magic) ||
        fd_magic[0] != 0x7f || fd_magic[1] != 'E' || fd_magic[2] != 'L' || fd_magic[3] != 'F') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test read error", fd_read.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test read value", fd_read.value);
        return false;
    }
    auto fd_seek = dispatch(static_cast<uint64_t>(Number::Seek), fd_open.value, 0, 0, 0);
    unsigned char seek_magic[4]{};
    auto seek_read = dispatch(static_cast<uint64_t>(Number::Read), fd_open.value, reinterpret_cast<uint64_t>(seek_magic), sizeof(seek_magic), 0);
    if (fd_seek.error != kErrorNone || fd_seek.value != 1 ||
        seek_read.error != kErrorNone || seek_read.value != sizeof(seek_magic) ||
        seek_magic[0] != 0x7f || seek_magic[1] != 'E' || seek_magic[2] != 'L' || seek_magic[3] != 'F') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test seek error", fd_seek.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test seek value", fd_seek.value);
        return false;
    }
    auto fd_close = dispatch(static_cast<uint64_t>(Number::Close), fd_open.value, 0, 0, 0);
    if (fd_close.error != kErrorNone || fd_close.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test close error", fd_close.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test close value", fd_close.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall seek self-test");
    auto device_count = dispatch(static_cast<uint64_t>(Number::GetDeviceCount), 0, 0, 0, 0);
    auto storage_count = dispatch(static_cast<uint64_t>(Number::GetStorageDeviceCount), 0, 0, 0, 0);
    auto network_count = dispatch(static_cast<uint64_t>(Number::GetNetworkDeviceCount), 0, 0, 0, 0);
    auto display_count = dispatch(static_cast<uint64_t>(Number::GetDisplayDeviceCount), 0, 0, 0, 0);
    if (device_count.error != kErrorNone || storage_count.error != kErrorNone || network_count.error != kErrorNone || display_count.error != kErrorNone ||
        device_count.value < 3 || storage_count.value == 0 || network_count.value == 0 || display_count.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test device-count value", device_count.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test storage-count value", storage_count.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test network-count value", network_count.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test display-count value", display_count.value);
        return false;
    }
    hybrid::DeviceInfo first_device{};
    auto device_info = dispatch(static_cast<uint64_t>(Number::GetDeviceInfo), 0, reinterpret_cast<uint64_t>(&first_device), 0, 0);
    if (device_info.error != kErrorNone || device_info.value != 1 ||
        first_device.device_class == hybrid::DeviceClass::Unknown ||
        first_device.resource_count == 0 ||
        first_device.resources[0].type == hybrid::DeviceResourceType::None ||
        first_device.resources[0].base == 0 ||
        first_device.resources[0].size == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test device-info error", device_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test device-info value", device_info.value);
        return false;
    }
    auto missing_device = dispatch(static_cast<uint64_t>(Number::GetDeviceInfo), device_count.value, reinterpret_cast<uint64_t>(&first_device), 0, 0);
    if (missing_device.error != kErrorNotFound || missing_device.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test missing-device error", missing_device.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test missing-device value", missing_device.value);
        return false;
    }
    hybrid::DeviceInfo storage_device{};
    hybrid::DeviceInfo network_device{};
    hybrid::DeviceInfo display_device{};
    auto storage_info = dispatch(static_cast<uint64_t>(Number::GetDeviceInfoByClass), static_cast<uint64_t>(hybrid::DeviceClass::Storage), 0, reinterpret_cast<uint64_t>(&storage_device), 0);
    auto network_info = dispatch(static_cast<uint64_t>(Number::GetDeviceInfoByClass), static_cast<uint64_t>(hybrid::DeviceClass::Network), 0, reinterpret_cast<uint64_t>(&network_device), 0);
    auto display_info = dispatch(static_cast<uint64_t>(Number::GetDeviceInfoByClass), static_cast<uint64_t>(hybrid::DeviceClass::Display), 0, reinterpret_cast<uint64_t>(&display_device), 0);
    if (storage_info.error != kErrorNone || network_info.error != kErrorNone || display_info.error != kErrorNone ||
        storage_device.device_class != hybrid::DeviceClass::Storage ||
        network_device.device_class != hybrid::DeviceClass::Network ||
        display_device.device_class != hybrid::DeviceClass::Display) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test storage-info error", storage_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test network-info error", network_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test display-info error", display_info.error);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall device inventory self-test");
    hybrid::FramebufferInfo framebuffer{};
    auto framebuffer_info = dispatch(static_cast<uint64_t>(Number::GetFramebufferInfo), reinterpret_cast<uint64_t>(&framebuffer), 0, 0, 0);
    if (framebuffer_info.error != kErrorNone || framebuffer_info.value != 1 ||
        framebuffer.base == 0 || framebuffer.width == 0 || framebuffer.height == 0 ||
        framebuffer.pixels_per_scanline < framebuffer.width || framebuffer.bytes_per_pixel < 4) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test framebuffer-info error", framebuffer_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test framebuffer-info value", framebuffer_info.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall framebuffer self-test");
    hybrid::MemoryStatsInfo memory_stats{};
    auto memory_info = dispatch(static_cast<uint64_t>(Number::GetMemoryStats), reinterpret_cast<uint64_t>(&memory_stats), 0, 0, 0);
    if (memory_info.error != kErrorNone || memory_info.value != 1 ||
        memory_stats.total_pages == 0 || memory_stats.free_pages == 0 ||
        memory_stats.used_pages == 0 || memory_stats.highest_physical == 0 ||
        memory_stats.free_pages + memory_stats.used_pages != memory_stats.total_pages ||
        memory_stats.usable_bytes == 0 || memory_stats.reserved_bytes == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test memory-stats error", memory_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test memory-stats value", memory_info.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test memory total pages", memory_stats.total_pages);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test memory free pages", memory_stats.free_pages);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test memory used pages", memory_stats.used_pages);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall memory stats self-test");
    hybrid::BlockDeviceInfo block_info{};
    auto block_info_result = dispatch(static_cast<uint64_t>(Number::GetBlockDeviceInfo), reinterpret_cast<uint64_t>(&block_info), 0, 0, 0);
    unsigned char block_sector[512]{};
    auto block_read_result = dispatch(static_cast<uint64_t>(Number::ReadBlockSector), 0, reinterpret_cast<uint64_t>(block_sector), 0, 0);
    if (block_info_result.error != kErrorNone || block_info_result.value != 1 ||
        block_info.initialized == 0 || block_info.sector_size != 512 || block_info.sector_count == 0 ||
        block_info.cache_fills == 0 || block_info.cached_entries == 0 ||
        block_info.backend_read_failures != 0 ||
        block_read_result.error != kErrorNone || block_read_result.value != 512 ||
        block_sector[510] != 0x55 || block_sector[511] != 0xaa) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test block-info error", block_info_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test block-info initialized", block_info.initialized);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test block-read error", block_read_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test block-read bytes", block_read_result.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall block device self-test");
    hybrid::SystemInfo system_info{};
    auto system_result = dispatch(static_cast<uint64_t>(Number::GetSystemInfo), reinterpret_cast<uint64_t>(&system_info), 0, 0, 0);
    if (system_result.error != kErrorNone || system_result.value != 1 ||
        system_info.boot_info_version == 0 || system_info.boot_module_count == 0 ||
        system_info.kernel_base == 0 || system_info.kernel_end <= system_info.kernel_base ||
        system_info.kernel_entry < system_info.kernel_base || system_info.kernel_entry >= system_info.kernel_end ||
        system_info.sysname[0] != 'I' || system_info.machine[0] != 'x' ||
        system_info.boot_mode[0] != 'u' || system_info.kernel_type[0] != 'M') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test system-info error", system_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test system-info value", system_result.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test system-info modules", system_info.boot_module_count);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall system info self-test");
    hybrid::LimitsInfo limits_info{};
    auto limits_result = dispatch(static_cast<uint64_t>(Number::GetLimitsInfo), reinterpret_cast<uint64_t>(&limits_info), 0, 0, 0);
    if (limits_result.error != kErrorNone || limits_result.value != 1 ||
        limits_info.max_boot_modules < system_info.boot_module_count ||
        limits_info.max_vfs_nodes < hk::fs::vfs().node_count() ||
        limits_info.max_process_file_descriptors < 3 ||
        limits_info.max_pipes == 0 || limits_info.pipe_capacity == 0 ||
        limits_info.max_cpus < system_info.boot_info_version ||
        limits_info.pmm_bitmap_pages == 0 ||
        limits_info.mounted_fat_path_capacity < limits_info.max_boot_modules) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test limits-info error", limits_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test limits-info value", limits_result.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test limits-info boot modules", limits_info.max_boot_modules);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall limits info self-test");
    hybrid::AbiInfo abi_info{};
    auto abi_result = dispatch(static_cast<uint64_t>(Number::GetAbiInfo), reinterpret_cast<uint64_t>(&abi_info), 0, 0, 0);
    if (abi_result.error != kErrorNone || abi_result.value != 1 ||
        abi_info.abi_version != hybrid::kSyscallAbiVersion ||
        abi_info.boot_info_version != hybrid::kBootInfoVersion ||
        abi_info.syscall_max_number != hybrid::kSyscallMaxNumber ||
        abi_info.boot_info_size != sizeof(hybrid::BootInfo) ||
        abi_info.abi_info_size != sizeof(hybrid::AbiInfo) ||
        abi_info.feature_info_size != sizeof(hybrid::FeatureInfo) ||
        abi_info.system_info_size != sizeof(hybrid::SystemInfo) ||
        abi_info.limits_info_size != sizeof(hybrid::LimitsInfo)) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test abi-info error", abi_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test abi-info value", abi_result.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test abi max", abi_info.syscall_max_number);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall abi info self-test");
    hybrid::FeatureInfo feature_info{};
    auto feature_result = dispatch(static_cast<uint64_t>(Number::GetFeatureInfo), reinterpret_cast<uint64_t>(&feature_info), 0, 0, 0);
    constexpr uint64_t required_features =
        hybrid::KernelFeatureUefiBoot |
        hybrid::KernelFeatureSyscalls |
        hybrid::KernelFeatureVfs |
        hybrid::KernelFeatureElfUserspace |
        hybrid::KernelFeatureScheduler |
        hybrid::KernelFeaturePci;
    if (feature_result.error != kErrorNone || feature_result.value != 1 ||
        (feature_info.flags & required_features) != required_features ||
        feature_info.stable_count < 6 || feature_info.experimental_count != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test feature-info error", feature_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test feature-info value", feature_result.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test feature flags", feature_info.flags);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall feature info self-test");
    char kernel_log_sample[128]{};
    auto kernel_log_read = dispatch(static_cast<uint64_t>(Number::ReadKernelLog), reinterpret_cast<uint64_t>(kernel_log_sample), sizeof(kernel_log_sample), 0, 0);
    if (kernel_log_read.error != kErrorNone || kernel_log_read.value == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test kernel-log error", kernel_log_read.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test kernel-log bytes", kernel_log_read.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall kernel log self-test");
    hybrid::ProcessInfo process_info{};
    auto process_snapshot = dispatch(static_cast<uint64_t>(Number::GetProcessInfo), 0, reinterpret_cast<uint64_t>(&process_info), 0, 0);
    if (process_snapshot.error != kErrorNone || process_snapshot.value != 1 ||
        process_info.pid == 0 || process_info.entry == 0 || process_info.address_space_root == 0 ||
        process_info.user_stack_top == 0 || process_info.main_thread_id == 0 ||
        process_info.owned_page_count == 0 || process_info.syscall_count == 0 ||
        process_info.last_syscall != static_cast<uint64_t>(Number::GetProcessInfo) ||
        process_info.name[0] == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-info error", process_snapshot.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-info value", process_snapshot.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-info pid", process_info.pid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-info syscalls", process_info.syscall_count);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test process-info last", process_info.last_syscall);
        return false;
    }
    hybrid::UserThreadInfo thread_info{};
    auto thread_snapshot = dispatch(static_cast<uint64_t>(Number::GetUserThreadInfo), 0, reinterpret_cast<uint64_t>(&thread_info), 0, 0);
    if (thread_snapshot.error != kErrorNone || thread_snapshot.value != 1 ||
        thread_info.tid == 0 || thread_info.pid != process_info.pid ||
        thread_info.entry != process_info.entry || thread_info.address_space_root != process_info.address_space_root ||
        thread_info.user_stack_pointer == 0 || thread_info.user_stack_pointer >= process_info.user_stack_top ||
        thread_info.syscall_count == 0 || thread_info.last_syscall != static_cast<uint64_t>(Number::GetUserThreadInfo)) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-info error", thread_snapshot.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-info value", thread_snapshot.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-info tid", thread_info.tid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-info syscalls", thread_info.syscall_count);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test thread-info last", thread_info.last_syscall);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall accounting self-test");
    if (process_info.run_ticks != 0 || thread_info.run_ticks != 0) {
        hk::log(hk::LogLevel::Info, "syscall runtime tick accounting self-test");
    } else {
        hk::log(hk::LogLevel::Info, "syscall runtime tick accounting initialized");
    }
    hk::log(hk::LogLevel::Info, "syscall process info self-test");
    hybrid::LaunchContextInfo launch_context{};
    auto launch_info = dispatch(static_cast<uint64_t>(Number::GetLaunchContext), thread_info.tid, reinterpret_cast<uint64_t>(&launch_context), 0, 0);
    if (launch_info.error != kErrorNone || launch_info.value != 1 ||
        launch_context.tid != thread_info.tid || launch_context.pid != process_info.pid ||
        launch_context.rip != process_info.entry || launch_context.rsp != thread_info.user_stack_pointer ||
        launch_context.cr3 != process_info.address_space_root || launch_context.cs == 0 || launch_context.ss == 0 ||
        (launch_context.rflags & 0x200) == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test launch-context error", launch_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test launch-context value", launch_info.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test launch-context tid", launch_context.tid);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall launch context self-test");
    hybrid::SchedulerStatsInfo scheduler_stats{};
    auto scheduler_info = dispatch(static_cast<uint64_t>(Number::GetSchedulerStats), reinterpret_cast<uint64_t>(&scheduler_stats), 0, 0, 0);
    if (scheduler_info.error != kErrorNone || scheduler_info.value != 1 ||
        scheduler_stats.thread_count == 0 || scheduler_stats.current_thread_id == 0 ||
        scheduler_stats.ready_count > scheduler_stats.thread_count ||
        scheduler_stats.sleeping_count > scheduler_stats.thread_count ||
        scheduler_stats.dead_count > scheduler_stats.thread_count ||
        scheduler_stats.online_cpu_count == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test scheduler-stats error", scheduler_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test scheduler-stats value", scheduler_info.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test scheduler-stats threads", scheduler_stats.thread_count);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall scheduler stats self-test");
    auto cpu_count = dispatch(static_cast<uint64_t>(Number::GetCpuCount), 0, 0, 0, 0);
    hybrid::CpuInfo cpu0{};
    auto cpu_info = dispatch(static_cast<uint64_t>(Number::GetCpuInfo), 0, reinterpret_cast<uint64_t>(&cpu0), 0, 0);
    if (cpu_count.error != kErrorNone || cpu_count.value == 0 ||
        cpu_info.error != kErrorNone || cpu_info.value != 1 ||
        (cpu0.flags & hybrid::CpuInfoEnabled) == 0 ||
        (cpu0.flags & hybrid::CpuInfoOnline) == 0 ||
        (cpu0.flags & hybrid::CpuInfoBootstrap) == 0 ||
        (cpu0.flags & hybrid::CpuInfoScheduler) == 0 ||
        (cpu0.flags & hybrid::CpuInfoDescriptorsReady) == 0 ||
        scheduler_stats.current_cpu_id >= cpu_count.value) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu-count error", cpu_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu-count value", cpu_count.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu-info error", cpu_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu-flags", cpu0.flags);
        return false;
    }
    if (cpu_count.value > 1) {
        hybrid::CpuInfo cpu1{};
        auto cpu1_info = dispatch(static_cast<uint64_t>(Number::GetCpuInfo), 1, reinterpret_cast<uint64_t>(&cpu1), 0, 0);
        if (cpu1_info.error != kErrorNone || cpu1_info.value != 1 ||
            (cpu1.flags & hybrid::CpuInfoEnabled) == 0 ||
            (cpu1.flags & hybrid::CpuInfoOnline) == 0 ||
            (cpu1.flags & hybrid::CpuInfoStartupAttempted) == 0 ||
            (cpu1.flags & hybrid::CpuInfoParked) == 0 ||
            (cpu1.flags & hybrid::CpuInfoDescriptorsReady) == 0 ||
            (cpu1.flags & hybrid::CpuInfoLocalApicTimerReady) == 0 ||
            (cpu1.flags & hybrid::CpuInfoBootstrapWorkDone) == 0 ||
            (cpu1.flags & hybrid::CpuInfoIpiWorkDone) == 0) {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu1-info error", cpu1_info.error);
            hk::log_hex(hk::LogLevel::Error, "syscall self-test cpu1-flags", cpu1.flags);
            return false;
        }
        if (hk::smp::queued_work_completed() < 3 ||
            hk::smp::work_counter(1) < 2 ||
            hk::smp::tlb_shootdown_completed() < 2 ||
            hk::smp::tlb_shootdown_counter(1) < 2) {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test smp queued work", hk::smp::queued_work_completed());
            hk::log_hex(hk::LogLevel::Error, "syscall self-test smp work counter", hk::smp::work_counter(1));
            hk::log_hex(hk::LogLevel::Error, "syscall self-test smp tlb shootdown", hk::smp::tlb_shootdown_completed());
            hk::log_hex(hk::LogLevel::Error, "syscall self-test smp tlb counter", hk::smp::tlb_shootdown_counter(1));
            return false;
        }
    }
    hk::log(hk::LogLevel::Info, "syscall CPU topology self-test");
    auto vfs_nodes = dispatch(static_cast<uint64_t>(Number::GetVfsNodeCount), 0, 0, 0, 0);
    if (vfs_nodes.error != kErrorNone || vfs_nodes.value < 5) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-node-count error", vfs_nodes.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-node-count value", vfs_nodes.value);
        return false;
    }
    bool saw_directory = false;
    bool saw_init = false;
    hybrid::VfsNodeInfo node_info{};
    for (uint64_t i = 0; i < vfs_nodes.value; ++i) {
        auto node_result = dispatch(static_cast<uint64_t>(Number::GetVfsNodeInfo), i, reinterpret_cast<uint64_t>(&node_info), 0, 0);
        if (node_result.error != kErrorNone || node_result.value != 1 || node_info.path[0] != '/') {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-node-info error", node_result.error);
            hk::log_hex(hk::LogLevel::Error, "syscall self-test vfs-node-info value", node_result.value);
            return false;
        }
        if (node_info.type == hybrid::VfsNodeType::Directory) saw_directory = true;
        if (node_info.type == hybrid::VfsNodeType::MemoryFile && node_info.base != 0 && node_info.size >= 4 &&
            node_info.path[0] == '/' && node_info.path[1] == 'u') {
            saw_init = true;
        }
    }
    if (!saw_directory || !saw_init) {
        hk::log(hk::LogLevel::Error, "syscall self-test vfs-node scan FAIL");
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall vfs node self-test");
    static const char bin_dir[] = "/bin";
    hybrid::VfsDirectoryEntryInfo dir_entry{};
    auto dir_result = dispatch(static_cast<uint64_t>(Number::ReadDirectory), reinterpret_cast<uint64_t>(bin_dir), sizeof(bin_dir), 0, reinterpret_cast<uint64_t>(&dir_entry));
    if (dir_result.error != kErrorNone || dir_result.value != 1 ||
        dir_entry.type != hybrid::VfsNodeType::MemoryFile ||
        dir_entry.path[0] != '/' || dir_entry.path[1] != 'b' || dir_entry.name[0] == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test readdir error", dir_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test readdir value", dir_result.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall readdir self-test");
    auto mount_count = dispatch(static_cast<uint64_t>(Number::GetMountCount), 0, 0, 0, 0);
    if (mount_count.error != kErrorNone || mount_count.value < 2) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test mount-count error", mount_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test mount-count value", mount_count.value);
        return false;
    }
    bool saw_root_mount = false;
    bool saw_fat_mount = false;
    hybrid::MountInfo mount_info{};
    for (uint64_t i = 0; i < mount_count.value; ++i) {
        auto mount_result = dispatch(static_cast<uint64_t>(Number::GetMountInfo), i, reinterpret_cast<uint64_t>(&mount_info), 0, 0);
        if (mount_result.error != kErrorNone || mount_result.value != 1 || mount_info.path[0] != '/') {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test mount-info error", mount_result.error);
            hk::log_hex(hk::LogLevel::Error, "syscall self-test mount-info value", mount_result.value);
            return false;
        }
        if (mount_info.path[0] == '/' && mount_info.path[1] == 0 &&
            (mount_info.flags & hybrid::MountMemoryBacked) != 0) {
            saw_root_mount = true;
        }
        if (mount_info.path[0] == '/' && mount_info.path[1] == 'm' &&
            (mount_info.flags & hybrid::MountDiskBacked) != 0 && mount_info.total_bytes != 0) {
            saw_fat_mount = true;
        }
    }
    if (!saw_root_mount || !saw_fat_mount) {
        hk::log(hk::LogLevel::Error, "syscall self-test mount scan FAIL");
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall mount table self-test");
    auto current_pid = dispatch(static_cast<uint64_t>(Number::GetCurrentProcessId), 0, 0, 0, 0);
    if (current_pid.error != kErrorNone || current_pid.value != process_info.pid) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test current-pid error", current_pid.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test current-pid value", current_pid.value);
        return false;
    }
    hybrid::PathInfo cwd{};
    auto cwd_info = dispatch(static_cast<uint64_t>(Number::GetCurrentDirectory), reinterpret_cast<uint64_t>(&cwd), 0, 0, 0);
    if (cwd_info.error != kErrorNone || cwd_info.value != 1 || cwd.path[0] != '/' || cwd.path[1] != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test get-cwd error", cwd_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test get-cwd value", cwd_info.value);
        return false;
    }
    static const char user_dir[] = "/user";
    auto set_user_dir = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(user_dir), sizeof(user_dir), 0, 0);
    cwd = hybrid::PathInfo{};
    cwd_info = dispatch(static_cast<uint64_t>(Number::GetCurrentDirectory), reinterpret_cast<uint64_t>(&cwd), 0, 0, 0);
    if (set_user_dir.error != kErrorNone || set_user_dir.value != 1 ||
        cwd_info.error != kErrorNone || cwd.path[0] != '/' || cwd.path[1] != 'u' || cwd.path[2] != 's' ||
        cwd.path[3] != 'e' || cwd.path[4] != 'r' || cwd.path[5] != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test set-cwd error", set_user_dir.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test set-cwd value", set_user_dir.value);
        return false;
    }
    static const char relative_tmp[] = "workdir";
    auto make_relative_dir = dispatch(static_cast<uint64_t>(Number::CreateDirectory), reinterpret_cast<uint64_t>(relative_tmp), sizeof(relative_tmp), 0, 0);
    auto set_relative_dir = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(relative_tmp), sizeof(relative_tmp), 0, 0);
    cwd = hybrid::PathInfo{};
    cwd_info = dispatch(static_cast<uint64_t>(Number::GetCurrentDirectory), reinterpret_cast<uint64_t>(&cwd), 0, 0, 0);
    static const char parent_user[] = "/user";
    auto restore_user = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(parent_user), sizeof(parent_user), 0, 0);
    auto remove_relative_dir = dispatch(static_cast<uint64_t>(Number::DeleteDirectory), reinterpret_cast<uint64_t>(relative_tmp), sizeof(relative_tmp), 0, 0);
    if (make_relative_dir.error != kErrorNone || make_relative_dir.value != 1 ||
        set_relative_dir.error != kErrorNone || set_relative_dir.value != 1 ||
        cwd_info.error != kErrorNone || cwd.path[0] != '/' || cwd.path[1] != 'u' || cwd.path[2] != 's' ||
        cwd.path[3] != 'e' || cwd.path[4] != 'r' || cwd.path[5] != '/' || cwd.path[6] != 'w' ||
        restore_user.error != kErrorNone || remove_relative_dir.error != kErrorNone || remove_relative_dir.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test directory syscall error", make_relative_dir.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test directory cwd error", cwd_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test directory rmdir error", remove_relative_dir.error);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall directory self-test");
    auto set_file = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(init_path), sizeof(init_path), 0, 0);
    if (set_file.error != kErrorNotFound || set_file.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test set-file-cwd error", set_file.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test set-file-cwd value", set_file.value);
        return false;
    }
    static const char root_dir[] = "/";
    auto restore_root = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(root_dir), sizeof(root_dir), 0, 0);
    if (restore_root.error != kErrorNone || restore_root.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test restore-cwd error", restore_root.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test restore-cwd value", restore_root.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall cwd self-test");
    static const char link_source[] = "/tmp/syslink.txt";
    static const char link_alias[] = "/tmp/syslink.alias";
    static const char link_payload[] = "link-syscall\n";
    auto create_link_source = dispatch(static_cast<uint64_t>(Number::CreateFile), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), 0, 0);
    auto link_source_fd = dispatch(static_cast<uint64_t>(Number::Open), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), 0, 0);
    auto link_write = dispatch(static_cast<uint64_t>(Number::WriteFile), link_source_fd.value, reinterpret_cast<uint64_t>(link_payload), sizeof(link_payload) - 1, 0);
    auto close_link_source = dispatch(static_cast<uint64_t>(Number::Close), link_source_fd.value, 0, 0, 0);
    auto link_created = dispatch(static_cast<uint64_t>(Number::Link), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), reinterpret_cast<uint64_t>(link_alias), sizeof(link_alias));
    hybrid::VfsStatInfo source_link_stat{};
    hybrid::VfsStatInfo alias_link_stat{};
    auto source_link_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), reinterpret_cast<uint64_t>(&source_link_stat), 0);
    auto alias_link_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(link_alias), sizeof(link_alias), reinterpret_cast<uint64_t>(&alias_link_stat), 0);
    auto delete_alias = dispatch(static_cast<uint64_t>(Number::DeleteFile), reinterpret_cast<uint64_t>(link_alias), sizeof(link_alias), 0, 0);
    hybrid::VfsStatInfo source_after_unlink{};
    auto source_after_unlink_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), reinterpret_cast<uint64_t>(&source_after_unlink), 0);
    auto delete_link_source = dispatch(static_cast<uint64_t>(Number::DeleteFile), reinterpret_cast<uint64_t>(link_source), sizeof(link_source), 0, 0);
    if (create_link_source.error != kErrorNone || create_link_source.value != 1 ||
        link_source_fd.error != kErrorNone || link_source_fd.value < 3 ||
        link_write.error != kErrorNone || link_write.value != sizeof(link_payload) - 1 ||
        close_link_source.error != kErrorNone || close_link_source.value != 1 ||
        link_created.error != kErrorNone || link_created.value != 1 ||
        source_link_info.error != kErrorNone || source_link_stat.links != 2 ||
        alias_link_info.error != kErrorNone || alias_link_stat.links != 2 ||
        delete_alias.error != kErrorNone || delete_alias.value != 1 ||
        source_after_unlink_info.error != kErrorNone || source_after_unlink.links != 1 ||
        delete_link_source.error != kErrorNone || delete_link_source.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test link create error", link_created.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test link source links", source_link_stat.links);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test link alias links", alias_link_stat.links);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test link after links", source_after_unlink.links);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall link self-test");
    static const char trunc_path[] = "/tmp/systrunc.txt";
    static const char trunc_payload[] = "truncate-syscall\n";
    auto create_trunc = dispatch(static_cast<uint64_t>(Number::CreateFile), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), 0, 0);
    auto trunc_fd = dispatch(static_cast<uint64_t>(Number::Open), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), 0, 0);
    auto trunc_write = dispatch(static_cast<uint64_t>(Number::WriteFile), trunc_fd.value, reinterpret_cast<uint64_t>(trunc_payload), sizeof(trunc_payload) - 1, 0);
    auto close_trunc = dispatch(static_cast<uint64_t>(Number::Close), trunc_fd.value, 0, 0, 0);
    auto shrink_trunc = dispatch(static_cast<uint64_t>(Number::Truncate), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), 4, 0);
    hybrid::VfsStatInfo shrink_trunc_stat{};
    auto shrink_trunc_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), reinterpret_cast<uint64_t>(&shrink_trunc_stat), 0);
    auto extend_trunc = dispatch(static_cast<uint64_t>(Number::Truncate), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), 8, 0);
    hybrid::VfsStatInfo extend_trunc_stat{};
    auto extend_trunc_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), reinterpret_cast<uint64_t>(&extend_trunc_stat), 0);
    auto oversize_trunc = dispatch(static_cast<uint64_t>(Number::Truncate), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), hk::fs::kMaxRamFileBytes + 1, 0);
    auto delete_trunc = dispatch(static_cast<uint64_t>(Number::DeleteFile), reinterpret_cast<uint64_t>(trunc_path), sizeof(trunc_path), 0, 0);
    if (create_trunc.error != kErrorNone || create_trunc.value != 1 ||
        trunc_fd.error != kErrorNone || trunc_fd.value < 3 ||
        trunc_write.error != kErrorNone || trunc_write.value != sizeof(trunc_payload) - 1 ||
        close_trunc.error != kErrorNone || close_trunc.value != 1 ||
        shrink_trunc.error != kErrorNone || shrink_trunc.value != 1 ||
        shrink_trunc_info.error != kErrorNone || shrink_trunc_stat.size != 4 ||
        extend_trunc.error != kErrorNone || extend_trunc.value != 1 ||
        extend_trunc_info.error != kErrorNone || extend_trunc_stat.size != 8 ||
        oversize_trunc.error != kErrorNotFound || oversize_trunc.value != 0 ||
        delete_trunc.error != kErrorNone || delete_trunc.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test truncate shrink error", shrink_trunc.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test truncate shrink size", shrink_trunc_stat.size);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test truncate extend size", extend_trunc_stat.size);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test truncate oversize error", oversize_trunc.error);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall truncate self-test");
    static const char rename_source[] = "/tmp/sysrename.txt";
    static const char rename_dest[] = "/tmp/sysrenamed.txt";
    static const char rename_payload[] = "rename-syscall\n";
    auto create_rename = dispatch(static_cast<uint64_t>(Number::CreateFile), reinterpret_cast<uint64_t>(rename_source), sizeof(rename_source), 0, 0);
    auto rename_fd = dispatch(static_cast<uint64_t>(Number::Open), reinterpret_cast<uint64_t>(rename_source), sizeof(rename_source), 0, 0);
    auto rename_write = dispatch(static_cast<uint64_t>(Number::WriteFile), rename_fd.value, reinterpret_cast<uint64_t>(rename_payload), sizeof(rename_payload) - 1, 0);
    auto close_rename = dispatch(static_cast<uint64_t>(Number::Close), rename_fd.value, 0, 0, 0);
    auto rename_result = dispatch(static_cast<uint64_t>(Number::Rename), reinterpret_cast<uint64_t>(rename_source), sizeof(rename_source), reinterpret_cast<uint64_t>(rename_dest), sizeof(rename_dest));
    hybrid::VfsStatInfo renamed_stat{};
    auto renamed_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(rename_dest), sizeof(rename_dest), reinterpret_cast<uint64_t>(&renamed_stat), 0);
    hybrid::VfsStatInfo old_rename_stat{};
    auto old_rename_info = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(rename_source), sizeof(rename_source), reinterpret_cast<uint64_t>(&old_rename_stat), 0);
    auto delete_renamed = dispatch(static_cast<uint64_t>(Number::DeleteFile), reinterpret_cast<uint64_t>(rename_dest), sizeof(rename_dest), 0, 0);
    if (create_rename.error != kErrorNone || create_rename.value != 1 ||
        rename_fd.error != kErrorNone || rename_fd.value < 3 ||
        rename_write.error != kErrorNone || rename_write.value != sizeof(rename_payload) - 1 ||
        close_rename.error != kErrorNone || close_rename.value != 1 ||
        rename_result.error != kErrorNone || rename_result.value != 1 ||
        renamed_info.error != kErrorNone || renamed_stat.size != sizeof(rename_payload) - 1 ||
        old_rename_info.error != kErrorNotFound || old_rename_info.value != 0 ||
        delete_renamed.error != kErrorNone || delete_renamed.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test rename error", rename_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test rename size", renamed_stat.size);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test old rename error", old_rename_info.error);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall rename self-test");
    static const char tmp_dir[] = "/tmp";
    hybrid::VfsStatInfo tmp_stat{};
    auto tmp_stat_result = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(tmp_dir), sizeof(tmp_dir), reinterpret_cast<uint64_t>(&tmp_stat), 0);
    if (tmp_stat_result.error != kErrorNone || tmp_stat_result.value != 1 ||
        tmp_stat.type != hybrid::VfsNodeType::Directory ||
        (tmp_stat.flags & hybrid::VfsNodeDirectory) == 0 ||
        (tmp_stat.flags & hybrid::VfsNodeWritable) == 0 ||
        tmp_stat.path[0] != '/' || tmp_stat.path[1] != 't' || tmp_stat.path[2] != 'm' || tmp_stat.path[3] != 'p') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stat-info error", tmp_stat_result.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stat-info value", tmp_stat_result.value);
        return false;
    }
    auto relative_cwd = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(user_dir), sizeof(user_dir), 0, 0);
    static const char relative_init[] = "init.elf";
    static const char dotted_relative_init[] = "./../user/init.elf";
    hybrid::VfsStatInfo relative_stat{};
    auto relative_stat_result = dispatch(static_cast<uint64_t>(Number::VfsStatInfo), reinterpret_cast<uint64_t>(dotted_relative_init), sizeof(dotted_relative_init), reinterpret_cast<uint64_t>(&relative_stat), 0);
    auto relative_open = dispatch(static_cast<uint64_t>(Number::Open), reinterpret_cast<uint64_t>(relative_init), sizeof(relative_init), 0, 0);
    unsigned char relative_magic[4]{};
    auto relative_read = dispatch(static_cast<uint64_t>(Number::Read), relative_open.value, reinterpret_cast<uint64_t>(relative_magic), sizeof(relative_magic), 0);
    auto relative_close = dispatch(static_cast<uint64_t>(Number::Close), relative_open.value, 0, 0, 0);
    restore_root = dispatch(static_cast<uint64_t>(Number::SetCurrentDirectory), reinterpret_cast<uint64_t>(root_dir), sizeof(root_dir), 0, 0);
    if (relative_cwd.error != kErrorNone ||
        relative_stat_result.error != kErrorNone || relative_stat_result.value != 1 ||
        relative_stat.type != hybrid::VfsNodeType::MemoryFile ||
        (relative_stat.flags & hybrid::VfsNodeMemoryBacked) == 0 ||
        relative_stat.path[0] != '/' || relative_stat.path[1] != 'u' ||
        relative_open.error != kErrorNone || relative_open.value < 3 ||
        relative_read.error != kErrorNone || relative_read.value != sizeof(relative_magic) ||
        relative_magic[0] != 0x7f || relative_magic[1] != 'E' || relative_magic[2] != 'L' || relative_magic[3] != 'F' ||
        relative_close.error != kErrorNone || relative_close.value != 1 ||
        restore_root.error != kErrorNone) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test relative-open error", relative_open.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test relative-open value", relative_open.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall relative fd self-test");
    hk::log(hk::LogLevel::Info, "syscall fd self-test");
    static const char hello_path[] = "/bin/hello.elf arg0 arg1";
    uint64_t spawned_pid = 0;
    auto spawned = dispatch(static_cast<uint64_t>(Number::Spawn), reinterpret_cast<uint64_t>(hello_path), sizeof(hello_path), reinterpret_cast<uint64_t>(&spawned_pid), 0);
    if (spawned.error != kErrorNone || spawned.value == 0 || spawned_pid == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn error", spawned.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn value", spawned.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn pid", spawned_pid);
        return false;
    }
    auto spawned_processes = dispatch(static_cast<uint64_t>(Number::GetProcessCount), 0, 0, 0, 0);
    if (spawned_processes.error != kErrorNone || spawned_processes.value < 2) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn count error", spawned_processes.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn count value", spawned_processes.value);
        return false;
    }
    hybrid::ArgumentInfo spawned_arg0{};
    hybrid::ArgumentInfo spawned_arg1{};
    hybrid::ArgumentInfo spawned_arg2{};
    if (hk::userspace::userspace_manager().argument_count(spawned_pid) != 3 ||
        !hk::userspace::userspace_manager().copy_argument(spawned_pid, 0, spawned_arg0) ||
        !hk::userspace::userspace_manager().copy_argument(spawned_pid, 1, spawned_arg1) ||
        !hk::userspace::userspace_manager().copy_argument(spawned_pid, 2, spawned_arg2) ||
        spawned_arg0.value[0] != '/' || spawned_arg0.value[5] != 'h' ||
        spawned_arg1.value[0] != 'a' || spawned_arg1.value[3] != '0' ||
        spawned_arg2.value[0] != 'a' || spawned_arg2.value[3] != '1') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn argv pid", spawned_pid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn argv count", hk::userspace::userspace_manager().argument_count(spawned_pid));
        return false;
    }
    hybrid::UserSchedulerInfo user_sched{};
    auto user_sched_info = dispatch(static_cast<uint64_t>(Number::GetUserSchedulerInfo), reinterpret_cast<uint64_t>(&user_sched), 0, 0, 0);
    if (user_sched_info.error != kErrorNone || user_sched_info.value != 1 ||
        user_sched.current_tid == 0 || user_sched.current_pid == 0 || user_sched.runnable_threads == 0 ||
        user_sched.schedulable_threads < 2 || user_sched.timeslice_quantum == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched error", user_sched_info.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched current tid", user_sched.current_tid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched runnable", user_sched.runnable_threads);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched schedulable", user_sched.schedulable_threads);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-sched quantum", user_sched.timeslice_quantum);
        return false;
    }
    if (!hk::timer::user_preemption_enabled()) {
        hk::log(hk::LogLevel::Error, "syscall self-test preemption gate enable FAIL");
        return false;
    }
    hybrid::LaunchContextInfo selected_context{};
    auto selected = dispatch(static_cast<uint64_t>(Number::SelectNextUserThread), reinterpret_cast<uint64_t>(&selected_context), 0, 0, 0);
    if (selected.error != kErrorNone || selected.value == 0 || selected_context.pid != spawned_pid ||
        selected_context.rip == 0 || selected_context.rsp == 0 || selected_context.cr3 == 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-select error", selected.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-select value", selected.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test user-select pid", selected_context.pid);
        return false;
    }
    hk::userspace::UserExecutionContext parent_context{};
    if (!hk::userspace::userspace_manager().save_current_context(parent_context) ||
        !hk::userspace::userspace_manager().activate_thread(selected_context.tid) ||
        syscall_current_pid() != spawned_pid ||
        !hk::userspace::userspace_manager().restore_context(parent_context) ||
        syscall_current_pid() != parent_context.pid ||
        syscall_current_tid() != parent_context.tid) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test context current pid", syscall_current_pid());
        hk::log_hex(hk::LogLevel::Error, "syscall self-test context current tid", syscall_current_tid());
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall user context self-test");
    hk::log(hk::LogLevel::Info, "syscall user scheduler self-test");
    auto killed = dispatch(static_cast<uint64_t>(Number::Kill), spawned_pid, static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill), 0, 0);
    hybrid::ProcessInfo killed_info{};
    auto killed_snapshot = dispatch(static_cast<uint64_t>(Number::GetProcessInfo), 1, reinterpret_cast<uint64_t>(&killed_info), 0, 0);
    uint64_t waited_code = 0;
    auto waited = dispatch(static_cast<uint64_t>(Number::Wait), spawned_pid, reinterpret_cast<uint64_t>(&waited_code), 0, 0);
    auto reaped_spawn = dispatch(static_cast<uint64_t>(Number::ReapProcess), spawned_pid, 0, 0, 0);
    if (killed.error != kErrorNone || killed.value != 1 ||
        killed_snapshot.error != kErrorNone || killed_info.termination_reason != static_cast<uint32_t>(hybrid::ProcessTerminationReason::SigKill) ||
        waited.error != kErrorNone || waited.value != 1 || waited_code != 137 ||
        reaped_spawn.error != kErrorNone || reaped_spawn.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test spawn cleanup pid", spawned_pid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test kill error", killed.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test kill reason", killed_info.termination_reason);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test wait error", waited.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test wait code", waited_code);
        return false;
    }
    if (hk::timer::user_preemption_enabled()) {
        hk::log(hk::LogLevel::Error, "syscall self-test preemption gate disable FAIL");
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall user preemption gate self-test");
    static const char quoted_path[] = "/bin/args.elf \"two words\" 'single quoted' escaped\\ space";
    uint64_t quoted_pid = 0;
    auto quoted_spawned = dispatch(static_cast<uint64_t>(Number::Spawn), reinterpret_cast<uint64_t>(quoted_path), sizeof(quoted_path), reinterpret_cast<uint64_t>(&quoted_pid), hybrid::SpawnFlagStartSuspended);
    hybrid::ArgumentInfo quoted_arg1{};
    hybrid::ArgumentInfo quoted_arg2{};
    hybrid::ArgumentInfo quoted_arg3{};
    if (quoted_spawned.error != kErrorNone || quoted_spawned.value == 0 || quoted_pid == 0 ||
        hk::userspace::userspace_manager().argument_count(quoted_pid) != 4 ||
        !hk::userspace::userspace_manager().copy_argument(quoted_pid, 1, quoted_arg1) ||
        !hk::userspace::userspace_manager().copy_argument(quoted_pid, 2, quoted_arg2) ||
        !hk::userspace::userspace_manager().copy_argument(quoted_pid, 3, quoted_arg3) ||
        quoted_arg1.value[0] != 't' || quoted_arg1.value[3] != ' ' || quoted_arg1.value[4] != 'w' ||
        quoted_arg2.value[0] != 's' || quoted_arg2.value[6] != ' ' || quoted_arg2.value[7] != 'q' ||
        quoted_arg3.value[0] != 'e' || quoted_arg3.value[7] != ' ' || quoted_arg3.value[8] != 's') {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted spawn error", quoted_spawned.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted spawn pid", quoted_pid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted argc", hk::userspace::userspace_manager().argument_count(quoted_pid));
        return false;
    }
    auto quoted_killed = dispatch(static_cast<uint64_t>(Number::Kill), quoted_pid, static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill), 0, 0);
    uint64_t quoted_code = 0;
    auto quoted_waited = dispatch(static_cast<uint64_t>(Number::Wait), quoted_pid, reinterpret_cast<uint64_t>(&quoted_code), 0, 0);
    auto quoted_reaped = dispatch(static_cast<uint64_t>(Number::ReapProcess), quoted_pid, 0, 0, 0);
    if (quoted_killed.error != kErrorNone || quoted_waited.error != kErrorNone || quoted_reaped.error != kErrorNone) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted cleanup kill", quoted_killed.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted cleanup wait", quoted_waited.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test quoted cleanup reap", quoted_reaped.error);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall quoted argv self-test");
    hk::log(hk::LogLevel::Info, "syscall kill reason self-test");
    uint64_t term_pid = 0;
    auto term_spawned = dispatch(static_cast<uint64_t>(Number::Spawn), reinterpret_cast<uint64_t>(hello_path), sizeof(hello_path), reinterpret_cast<uint64_t>(&term_pid), 0);
    auto termed = dispatch(static_cast<uint64_t>(Number::Kill), term_pid, static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm), 0, 0);
    hybrid::ProcessInfo termed_info{};
    auto termed_snapshot = dispatch(static_cast<uint64_t>(Number::GetProcessInfo), 1, reinterpret_cast<uint64_t>(&termed_info), 0, 0);
    uint64_t termed_wait_code = 0;
    auto termed_waited = dispatch(static_cast<uint64_t>(Number::Wait), term_pid, reinterpret_cast<uint64_t>(&termed_wait_code), 0, 0);
    auto termed_reaped = dispatch(static_cast<uint64_t>(Number::ReapProcess), term_pid, 0, 0, 0);
    if (term_spawned.error != kErrorNone || term_spawned.value == 0 || term_pid == 0 ||
        termed.error != kErrorNone || termed.value != 1 ||
        termed_snapshot.error != kErrorNone || termed_info.termination_reason != static_cast<uint32_t>(hybrid::ProcessTerminationReason::SigTerm) ||
        termed_waited.error != kErrorNone || termed_waited.value != 1 || termed_wait_code != 143 ||
        termed_reaped.error != kErrorNone || termed_reaped.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test sigterm spawn error", term_spawned.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test sigterm pid", term_pid);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test sigterm kill error", termed.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test sigterm reason", termed_info.termination_reason);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test sigterm wait code", termed_wait_code);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall sigterm reason self-test");
    hk::log(hk::LogLevel::Info, "syscall spawn self-test");
    hk::log(hk::LogLevel::Info, "syscall wait reap self-test");
    auto arg_count = dispatch(static_cast<uint64_t>(Number::GetArgumentCount), 0, 0, 0, 0);
    hybrid::ArgumentInfo arg0{};
    hybrid::ArgumentInfo arg1{};
    auto arg0_info = dispatch(static_cast<uint64_t>(Number::GetArgument), 0, reinterpret_cast<uint64_t>(&arg0), 0, 0);
    auto arg1_info = dispatch(static_cast<uint64_t>(Number::GetArgument), 1, reinterpret_cast<uint64_t>(&arg1), 0, 0);
    auto missing_arg = dispatch(static_cast<uint64_t>(Number::GetArgument), arg_count.value, reinterpret_cast<uint64_t>(&arg1), 0, 0);
    bool boot_arg_ok = arg_count.value == 1 ||
        (arg_count.value == 2 && arg1_info.error == kErrorNone && arg1_info.value == 1 &&
         arg1.value[0] == '-' && arg1.value[1] == '-' && arg1.value[2] == 'b' && arg1.value[3] == 'o' &&
         arg1.value[4] == 'o' && arg1.value[5] == 't' && arg1.value[6] == 0);
    if (arg_count.error != kErrorNone || (arg_count.value != 1 && arg_count.value != 2) ||
        arg0_info.error != kErrorNone || arg0_info.value != 1 ||
        arg0.value[0] != 'i' || arg0.value[1] != 'n' || arg0.value[2] != 'i' || arg0.value[3] != 't' || arg0.value[4] != 0 ||
        !boot_arg_ok ||
        missing_arg.error != kErrorNotFound || missing_arg.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test argument-count error", arg_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test argument-count value", arg_count.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall argument self-test");
    auto env_count = dispatch(static_cast<uint64_t>(Number::GetEnvironmentCount), 0, 0, 0, 0);
    hybrid::EnvironmentInfo env0{};
    hybrid::EnvironmentInfo env1{};
    auto env0_info = dispatch(static_cast<uint64_t>(Number::GetEnvironment), 0, reinterpret_cast<uint64_t>(&env0), 0, 0);
    auto env1_info = dispatch(static_cast<uint64_t>(Number::GetEnvironment), 1, reinterpret_cast<uint64_t>(&env1), 0, 0);
    auto missing_env = dispatch(static_cast<uint64_t>(Number::GetEnvironment), env_count.value, reinterpret_cast<uint64_t>(&env1), 0, 0);
    if (env_count.error != kErrorNone || env_count.value != 2 ||
        env0_info.error != kErrorNone || env0_info.value != 1 ||
        env1_info.error != kErrorNone || env1_info.value != 1 ||
        env0.key[0] != 'R' || env0.key[1] != 'O' || env0.key[2] != 'O' || env0.key[3] != 'T' || env0.key[4] != 0 ||
        env0.value[0] != '/' || env0.value[1] != 0 ||
        env1.key[0] != 'P' || env1.key[1] != 'A' || env1.key[2] != 'T' || env1.key[3] != 'H' || env1.key[4] != 0 ||
        env1.value[0] != '/' || env1.value[1] != 'b' || env1.value[2] != 'i' || env1.value[3] != 'n' || env1.value[4] != 0 ||
        missing_env.error != kErrorNotFound || missing_env.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test environment-count error", env_count.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test environment-count value", env_count.value);
        return false;
    }
    static const char env_test_key[] = "TEST";
    static const char env_test_value[] = "mutable-environment-value-longer-than-the-old-forty-byte-limit";
    auto set_env = dispatch(static_cast<uint64_t>(Number::SetEnvironment), reinterpret_cast<uint64_t>(env_test_key), sizeof(env_test_key), reinterpret_cast<uint64_t>(env_test_value), sizeof(env_test_value));
    auto env_count_after_set = dispatch(static_cast<uint64_t>(Number::GetEnvironmentCount), 0, 0, 0, 0);
    hybrid::EnvironmentInfo env2{};
    auto env2_info = dispatch(static_cast<uint64_t>(Number::GetEnvironment), 2, reinterpret_cast<uint64_t>(&env2), 0, 0);
    auto unset_env = dispatch(static_cast<uint64_t>(Number::UnsetEnvironment), reinterpret_cast<uint64_t>(env_test_key), sizeof(env_test_key), 0, 0);
    auto env_count_after_unset = dispatch(static_cast<uint64_t>(Number::GetEnvironmentCount), 0, 0, 0, 0);
    if (set_env.error != kErrorNone || set_env.value != 1 ||
        env_count_after_set.error != kErrorNone || env_count_after_set.value != 3 ||
        env2_info.error != kErrorNone || env2_info.value != 1 ||
        env2.key[0] != 'T' || env2.key[1] != 'E' || env2.key[2] != 'S' || env2.key[3] != 'T' || env2.key[4] != 0 ||
        env2.value[0] != 'm' || env2.value[1] != 'u' || env2.value[40] != 'e' ||
        unset_env.error != kErrorNone || unset_env.value != 1 ||
        env_count_after_unset.error != kErrorNone || env_count_after_unset.value != 2) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test setenv error", set_env.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test setenv value", set_env.value);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test env count after set", env_count_after_set.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall environment self-test");
    auto empty_key = dispatch(static_cast<uint64_t>(Number::ReadKey), 0, 0, 0, 0);
    if (empty_key.error != kErrorNotFound || empty_key.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test read-key error", empty_key.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test read-key value", empty_key.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall read-key self-test");
    char stdin_byte = 0;
    auto empty_stdin = dispatch(static_cast<uint64_t>(Number::Read), hybrid::kStdinFd, reinterpret_cast<uint64_t>(&stdin_byte), 1, 0);
    if (empty_stdin.error != kErrorNotFound || empty_stdin.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stdin read error", empty_stdin.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test stdin read value", empty_stdin.value);
        return false;
    }
    if (hk::console_log_enabled()) {
        static const char stdout_message[] = "[selftest] stdout fd write syscall\n";
        auto stdout_write = dispatch(static_cast<uint64_t>(Number::Write), hybrid::kStdoutFd, reinterpret_cast<uint64_t>(stdout_message), sizeof(stdout_message) - 1, 0);
        if (stdout_write.error != kErrorNone || stdout_write.value != sizeof(stdout_message) - 1) {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test stdout write error", stdout_write.error);
            hk::log_hex(hk::LogLevel::Error, "syscall self-test stdout write value", stdout_write.value);
            return false;
        }
        hk::log(hk::LogLevel::Info, "syscall stdio fd self-test");
        static const char terminal_message[] = "[selftest] terminal write syscall\n";
        auto terminal_write = dispatch(static_cast<uint64_t>(Number::Write), reinterpret_cast<uint64_t>(terminal_message), sizeof(terminal_message) - 1, 0, 0);
        if (terminal_write.error != kErrorNone || terminal_write.value != sizeof(terminal_message) - 1) {
            hk::log_hex(hk::LogLevel::Error, "syscall self-test write error", terminal_write.error);
            hk::log_hex(hk::LogLevel::Error, "syscall self-test write value", terminal_write.value);
            return false;
        }
        hk::log(hk::LogLevel::Info, "syscall write self-test");
    } else {
        hk::log(hk::LogLevel::Info, "syscall visible write self-test skipped in quiet boot");
    }
    auto scroll_test = dispatch(static_cast<uint64_t>(Number::TerminalControl), static_cast<uint64_t>(hybrid::TerminalControlCommand::ScrollRelative), static_cast<uint64_t>(-1), 0, 0);
    auto bottom_test = dispatch(static_cast<uint64_t>(Number::TerminalControl), static_cast<uint64_t>(hybrid::TerminalControlCommand::ScrollToBottom), 0, 0, 0);
    auto reset_line_test = dispatch(static_cast<uint64_t>(Number::TerminalControl), static_cast<uint64_t>(hybrid::TerminalControlCommand::ResetInputLine), 0, 0, 0);
    if (scroll_test.error != kErrorNone || scroll_test.value != 1 ||
        bottom_test.error != kErrorNone || bottom_test.value != 1 ||
        reset_line_test.error != kErrorNone || reset_line_test.value != 1) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test terminal-control error", scroll_test.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test terminal-control value", scroll_test.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall terminal control self-test");
    auto retired_run_process = dispatch(52, 0, 0, 0, 0);
    if (retired_run_process.error != kErrorInvalidSyscall || retired_run_process.value != 0) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test retired run-process error", retired_run_process.error);
        hk::log_hex(hk::LogLevel::Error, "syscall self-test retired run-process value", retired_run_process.value);
        return false;
    }
    hk::log(hk::LogLevel::Info, "syscall retired run-process self-test");
    hk::log(hk::LogLevel::Info, "syscall vfs self-test");
    static const char message[] = "syscall debug-log self-test";
    auto logged = dispatch(static_cast<uint64_t>(Number::DebugLog), reinterpret_cast<uint64_t>(message), sizeof(message), 0, 0);
    if (logged.error != kErrorNone) {
        hk::log_hex(hk::LogLevel::Error, "syscall self-test debug-log error", logged.error);
        return false;
    }
    return true;
}

} // namespace hk::syscall
