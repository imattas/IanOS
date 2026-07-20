#pragma once
#include <stdint.h>
#include "hybrid/syscall.hpp"
namespace hk::userspace {
enum class ProcessState : uint8_t { Empty, Created, Runnable, Stopped, Exited };
enum class UserThreadState : uint8_t { Empty, Created, Runnable, Running, Blocked, Exited };
enum class UserBlockReason : uint8_t { None, PipeRead, PipeWrite, ProcessWait, Sleep };

constexpr uint64_t kDefaultUserStackTop = 0x0000007ffffff000ull;
constexpr uint64_t kDefaultUserStackPages = 4;
constexpr uint32_t kMaxUserProcesses = 16;
constexpr uint32_t kMaxUserThreads = 32;
constexpr uint32_t kMaxProcessFileDescriptors = 8;
constexpr uint32_t kMaxOwnedUserPages = 64;
constexpr uint32_t kMaxProcessArguments = 8;
constexpr uint32_t kMaxArgumentLength = 64;
constexpr uint32_t kMaxEnvironmentEntries = 4;
constexpr uint32_t kMaxEnvironmentKeyLength = 24;
constexpr uint32_t kMaxEnvironmentValueLength = 80;
constexpr uint32_t kMaxPipes = 8;
constexpr uint32_t kPipeCapacity = 1024;
constexpr uint64_t kUserTimesliceQuantum = 4;

enum class FileDescriptorKind : uint8_t { Empty, Vfs, PipeRead, PipeWrite };

struct FileDescriptor {
    uint32_t fd;
    uint32_t vfs_handle;
    uint32_t pipe_id;
    uint64_t offset;
    FileDescriptorKind kind;
    bool open;
    char path[64];
};

struct Pipe {
    uint32_t id;
    bool open;
    uint64_t size;
    uint64_t read_offset;
    uint8_t data[kPipeCapacity];
};

struct OwnedPage {
    uint64_t virt;
    uint64_t phys;
};

struct Process {
    uint64_t pid;
    uint64_t parent_pid;
    ProcessState state;
    const char* name;
    uint64_t entry;
    uint64_t address_space_root;
    uint64_t user_stack_top;
    uint64_t user_stack_pages;
    uint64_t image_base;
    uint64_t image_pages;
    uint64_t main_thread_id;
    uint64_t exit_code;
    hybrid::ProcessTerminationReason termination_reason;
    uint32_t next_fd;
    FileDescriptor file_descriptors[kMaxProcessFileDescriptors];
    uint64_t owned_page_count;
    OwnedPage owned_pages[kMaxOwnedUserPages];
    char current_directory[64];
    uint32_t argument_count;
    char arguments[kMaxProcessArguments][kMaxArgumentLength];
    uint32_t environment_count;
    char environment_keys[kMaxEnvironmentEntries][kMaxEnvironmentKeyLength];
    char environment_values[kMaxEnvironmentEntries][kMaxEnvironmentValueLength];
    bool owns_address_space;
    uint64_t process_group_id;
    uint64_t syscall_count;
    uint64_t last_syscall;
    uint64_t read_syscalls;
    uint64_t write_syscalls;
    uint64_t read_bytes;
    uint64_t write_bytes;
    uint64_t run_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
};

struct UserThread {
    uint64_t tid;
    uint64_t pid;
    UserThreadState state;
    UserBlockReason block_reason;
    uint32_t wait_pipe_id;
    uint64_t wait_process_id;
    uint64_t wait_wake_tick;
    uint32_t wait_fd;
    uint64_t wait_buffer;
    uint64_t wait_size;
    uint64_t syscall_count;
    uint64_t last_syscall;
    uint64_t run_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
    uint64_t entry;
    uint64_t user_stack_pointer;
    uint64_t address_space_root;
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t rflags;
};

struct UserLaunchContext {
    uint64_t tid;
    uint64_t pid;
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr3;
    uint16_t cs;
    uint16_t ss;
    uint64_t rflags;
};

struct UserExecutionContext {
    uint64_t tid;
    uint64_t pid;
    uint64_t cr3;
    bool valid;
};

struct UserspaceDiagnostics {
    uint64_t pipe_read_blocks;
    uint64_t pipe_write_blocks;
    uint64_t pipe_read_wakes;
    uint64_t pipe_write_wakes;
    uint64_t process_wait_blocks;
    uint64_t process_wait_any_blocks;
    uint64_t process_wait_wakes;
    uint64_t sleep_blocks;
    uint64_t sleep_wakes;
    uint64_t preemption_gate_enables;
    uint64_t preemption_gate_disables;
    uint64_t preempt_switches;
};

class UserspaceManager {
public:
    void initialize();
    bool enabled() const { return enabled_; }
    Process* create_process(const char* name, uint64_t entry);
    Process* create_process_from_elf(const char* name, uint64_t image_base, uint64_t image_size);
    Process* create_process_stub(const char* name, uint64_t entry, uint64_t address_space_root);
    Process* find_process(uint64_t pid);
    UserThread* find_thread(uint64_t tid);
    bool mark_runnable(uint64_t pid);
    bool set_parent(uint64_t pid, uint64_t parent_pid);
    bool set_process_group(uint64_t caller_pid, uint64_t pid, uint64_t process_group_id);
    uint64_t kill_process_group(uint64_t caller_pid, uint64_t process_group_id, uint64_t code, hybrid::ProcessTerminationReason reason);
    bool set_foreground_process_group(uint64_t caller_pid, uint64_t process_group_id);
    uint64_t foreground_process_group() const { return foreground_process_group_id_; }
    uint64_t stop_process_group(uint64_t caller_pid, uint64_t process_group_id);
    uint64_t continue_process_group(uint64_t caller_pid, uint64_t process_group_id);
    bool exit_process(uint64_t pid, uint64_t code, hybrid::ProcessTerminationReason reason = hybrid::ProcessTerminationReason::Exited);
    bool wait_process(uint64_t waiter_pid, uint64_t target_pid, uint64_t& exit_code) const;
    bool wait_any_process(uint64_t waiter_pid, hybrid::WaitAnyInfo& out) const;
    bool reap_exited(uint64_t pid);
    bool activate_thread(uint64_t tid);
    bool save_active_thread_frame(uint64_t rip, uint64_t rsp);
    bool mark_active_thread_runnable();
    bool block_active_thread_on_pipe(uint32_t pipe_id, bool write_end, uint32_t fd, uint64_t buffer, uint64_t size);
    bool block_active_thread_on_process(uint64_t target_pid);
    bool block_active_thread_on_any_process();
    bool block_active_thread_until(uint64_t wake_tick);
    uint64_t wake_pipe_waiters(uint32_t pipe_id, bool readers, bool writers);
    uint64_t wake_process_waiters(uint64_t target_pid, uint64_t exit_code);
    uint64_t wake_sleepers(uint64_t now_tick);
    bool resolve_pipe_fd(uint64_t pid, uint32_t fd, bool write_end, uint32_t& pipe_id) const;
    bool process_wait_would_block(uint64_t waiter_pid, uint64_t target_pid) const;
    bool process_wait_any_would_block(uint64_t waiter_pid) const;
    bool save_current_context(UserExecutionContext& out) const;
    bool restore_context(const UserExecutionContext& context);
    bool select_next_runnable_thread(UserLaunchContext& out);
    bool build_process_launch_context(uint64_t pid, UserLaunchContext& out);
    bool build_launch_context(uint64_t tid, UserLaunchContext& out);
    uint32_t open_file(uint64_t pid, const char* path);
    bool redirect_file(uint64_t pid, uint32_t target_fd, const char* path);
    bool redirect_file_append(uint64_t pid, uint32_t target_fd, const char* path);
    uint32_t create_pipe();
    bool attach_pipe_fd(uint64_t pid, uint32_t target_fd, uint32_t pipe_id, bool write_end);
    bool close_pipe(uint32_t pipe_id);
    bool has_open_file(uint64_t pid, uint32_t fd) const;
    bool is_pipe_fd(uint64_t pid, uint32_t fd) const;
    bool pipe_read_would_block(uint64_t pid, uint32_t fd) const;
    bool pipe_write_would_block(uint64_t pid, uint32_t fd) const;
    bool create_file(uint64_t pid, const char* path);
    bool create_directory(uint64_t pid, const char* path);
    bool link_file(uint64_t pid, const char* existing_path, const char* new_path);
    bool truncate_file(uint64_t pid, const char* path, uint64_t size);
    bool rename_path(uint64_t pid, const char* old_path, const char* new_path);
    uint64_t read_file(uint64_t pid, uint32_t fd, void* buffer, uint64_t size);
    uint64_t write_file(uint64_t pid, uint32_t fd, const void* buffer, uint64_t size);
    bool seek_file(uint64_t pid, uint32_t fd, uint64_t offset);
    bool close_file(uint64_t pid, uint32_t fd);
    uint32_t duplicate_file(uint64_t pid, uint32_t source_fd);
    bool duplicate_file_to(uint64_t pid, uint32_t source_fd, uint32_t target_fd);
    bool delete_file(uint64_t pid, const char* path);
    bool delete_directory(uint64_t pid, const char* path);
    bool stat_path(uint64_t pid, const char* path, hybrid::VfsStatInfo& out) const;
    bool copy_directory_entry(uint64_t pid, const char* path, uint32_t index, hybrid::VfsDirectoryEntryInfo& out) const;
    bool set_current_directory(uint64_t pid, const char* path);
    bool copy_current_directory(uint64_t pid, hybrid::PathInfo& out) const;
    bool set_arguments(uint64_t pid, const char* const* args, uint32_t count);
    uint64_t argument_count(uint64_t pid) const;
    bool copy_argument(uint64_t pid, uint32_t index, hybrid::ArgumentInfo& out) const;
    bool set_environment(uint64_t pid, const char* const* keys, const char* const* values, uint32_t count);
    bool set_environment_variable(uint64_t pid, const char* key, const char* value);
    bool unset_environment_variable(uint64_t pid, const char* key);
    bool inherit_environment(uint64_t child_pid, uint64_t parent_pid);
    bool inherit_standard_fds(uint64_t child_pid, uint64_t parent_pid);
    uint64_t environment_count(uint64_t pid) const;
    bool copy_environment(uint64_t pid, uint32_t index, hybrid::EnvironmentInfo& out) const;
    uint64_t open_file_count(uint64_t pid) const;
    uint64_t owned_page_count(uint64_t pid) const;
    bool copy_process_info(uint64_t index, hybrid::ProcessInfo& out) const;
    bool copy_thread_info(uint64_t index, hybrid::UserThreadInfo& out) const;
    bool copy_file_descriptor_info(uint64_t pid, uint64_t index, hybrid::FileDescriptorInfo& out) const;
    uint64_t pipe_count() const;
    bool copy_pipe_info(uint64_t index, hybrid::PipeInfo& out) const;
    bool copy_launch_context(uint64_t tid, hybrid::LaunchContextInfo& out);
    bool copy_user_scheduler_info(hybrid::UserSchedulerInfo& out) const;
    bool copy_current_user_context(hybrid::CurrentUserContextInfo& out) const;
    UserspaceDiagnostics diagnostics() const { return diagnostics_; }
    bool record_current_syscall(uint64_t number);
    bool note_current_user_tick();
    bool user_timeslice_expired();
    bool note_user_preempt_switch(uint64_t from_tid, uint64_t to_tid);
    uint64_t schedulable_thread_count() const;
    bool update_user_preemption_gate();
    uint64_t current_pid() const;
    uint64_t current_tid() const;
    uint64_t active_pid() const;
    uint64_t active_tid() const;
    Process* current_process();
    UserThread* current_thread();
    uint64_t process_count() const { return count_; }
    uint64_t live_process_count() const;
    uint64_t user_thread_count() const { return thread_count_; }
    uint64_t runnable_count() const;
    uint64_t exited_count() const;
    uint64_t runnable_thread_count() const;
    uint64_t running_thread_count() const;
    uint64_t exited_thread_count() const;
private:
    bool enabled_ = false;
    Process processes_[kMaxUserProcesses]{};
    UserThread threads_[kMaxUserThreads]{};
    Pipe pipes_[kMaxPipes]{};
    uint64_t count_ = 0;
    uint64_t thread_count_ = 0;
    uint64_t next_pid_ = 1;
    uint64_t next_tid_ = 1;
    uint64_t active_pid_ = 0;
    uint64_t active_tid_ = 0;
    uint64_t last_selected_tid_ = 0;
    uint64_t last_user_pick_index_ = 0;
    uint64_t current_slice_ticks_ = 0;
    uint64_t expired_slices_ = 0;
    uint64_t foreground_process_group_id_ = 0;
    UserspaceDiagnostics diagnostics_{};
    Process* allocate_process_slot();
    UserThread* allocate_thread_slot();
    UserThread* create_main_thread(Process& process);
    Pipe* find_pipe(uint32_t pipe_id);
    const Pipe* find_pipe(uint32_t pipe_id) const;
    bool pipe_has_live_writer(uint32_t pipe_id) const;
    bool pipe_has_live_reader(uint32_t pipe_id) const;
    bool cleanup_pipe_if_unreferenced(uint32_t pipe_id);
};
UserspaceManager& userspace_manager();
bool validate_process_mappings(const Process& process);
bool process_lifecycle_self_test();
bool launch_context_self_test();
bool file_descriptor_self_test();
}
