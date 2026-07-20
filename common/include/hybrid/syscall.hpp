#pragma once
#include <stdint.h>
#include "hybrid/boot_info.hpp"

namespace hybrid {

enum class SyscallNumber : uint64_t {
    DebugLog = 1,
    GetTicks = 2,
    Yield = 3,
    SleepTicks = 4,
    GetThreadId = 5,
    GetProcessCount = 6,
    GetThreadCount = 7,
    GetRunnableProcessCount = 8,
    GetExitedProcessCount = 9,
    GetUserThreadCount = 10,
    GetRunnableUserThreadCount = 11,
    GetLiveProcessCount = 12,
    ReapProcess = 13,
    VfsStat = 14,
    VfsRead = 15,
    VfsOpen = 16,
    VfsReadHandle = 17,
    VfsClose = 18,
    Open = 19,
    Read = 20,
    Close = 21,
    GetDeviceCount = 22,
    GetStorageDeviceCount = 23,
    GetNetworkDeviceCount = 24,
    GetDisplayDeviceCount = 25,
    GetDeviceInfo = 26,
    GetDeviceInfoByClass = 27,
    GetFramebufferInfo = 28,
    GetMemoryStats = 29,
    GetProcessInfo = 30,
    GetUserThreadInfo = 31,
    GetSchedulerStats = 32,
    GetLaunchContext = 33,
    GetVfsNodeCount = 34,
    GetVfsNodeInfo = 35,
    GetCurrentProcessId = 36,
    GetCurrentDirectory = 37,
    SetCurrentDirectory = 38,
    GetArgumentCount = 39,
    GetArgument = 40,
    GetEnvironmentCount = 41,
    GetEnvironment = 42,
    Seek = 43,
    ReadKey = 44,
    Write = 45,
    Spawn = 46,
    Exit = 47,
    Kill = 48,
    Wait = 49,
    GetUserSchedulerInfo = 50,
    SelectNextUserThread = 51,
    GetSystemInfo = 53,
    ReadKernelLog = 54,
    CreateFile = 55,
    WriteFile = 56,
    DeleteFile = 57,
    CreateDirectory = 58,
    DeleteDirectory = 59,
    RedirectProcessFd = 60,
    TerminalControl = 61,
    RedirectProcessFdAppend = 62,
    CreatePipe = 63,
    AttachPipeFd = 64,
    ClosePipe = 65,
    SetEnvironment = 66,
    UnsetEnvironment = 67,
    VfsStatInfo = 68,
    GetDateTime = 69,
    GetCurrentUserContext = 70,
    SetUserPreemption = 71,
    GetCpuCount = 72,
    GetCpuInfo = 73,
    Dup = 74,
    Dup2 = 75,
    GetFileDescriptorInfo = 76,
    WaitAny = 77,
    GetCurrentIds = 78,
    SetProcessGroup = 79,
    KillProcessGroup = 80,
    StartProcess = 81,
    SetForegroundProcessGroup = 82,
    GetForegroundProcessGroup = 83,
    StopProcessGroup = 84,
    ContinueProcessGroup = 85,
    GetBlockDeviceInfo = 86,
    ReadBlockSector = 87,
    GetMountCount = 88,
    GetMountInfo = 89,
    GetPipeCount = 90,
    GetPipeInfo = 91,
    Link = 92,
    Truncate = 93,
    Rename = 94,
    ReadDirectory = 95,
    ReadLink = 96,
    GetLimitsInfo = 97,
    GetAbiInfo = 98,
};

constexpr uint32_t kSyscallAbiVersion = 1;
constexpr uint64_t kSyscallMaxNumber = static_cast<uint64_t>(SyscallNumber::GetAbiInfo);

constexpr uint32_t kStdinFd = 0;
constexpr uint32_t kStdoutFd = 1;
constexpr uint32_t kStderrFd = 2;

struct SyscallResult {
    uint64_t value;
    uint64_t error;
};

enum class DeviceClass : uint32_t {
    Unknown = 0,
    Storage = 1,
    Network = 2,
    Display = 3,
};

enum class DeviceResourceType : uint32_t {
    None = 0,
    Mmio = 1,
    Io = 2,
};

enum class TerminalControlCommand : uint64_t {
    ScrollRelative = 1,
    ScrollToBottom = 2,
    SetInputMode = 3,
    GetInputMode = 4,
    InjectInput = 5,
    ResetInputLine = 6,
};

enum class TerminalInputMode : uint64_t {
    Raw = 0,
    Canonical = 1,
};

enum class PipeEndpoint : uint64_t {
    Read = 0,
    Write = 1,
};

enum SpawnFlags : uint64_t {
    SpawnFlagStartSuspended = 1u << 0,
};

enum class ProcessTerminationReason : uint32_t {
    None = 0,
    Exited = 1,
    SigTerm = 15,
    SigKill = 9,
};

struct [[gnu::packed]] DeviceResourceInfo {
    DeviceResourceType type;
    uint32_t reserved;
    uint64_t base;
    uint64_t size;
};

struct [[gnu::packed]] DeviceInfo {
    DeviceClass device_class;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint8_t resource_count;
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t required_command_bits;
    uint16_t reserved;
    DeviceResourceInfo resources[3];
};

struct [[gnu::packed]] MemoryStatsInfo {
    uint64_t total_pages;
    uint64_t free_pages;
    uint64_t used_pages;
    uint64_t usable_bytes;
    uint64_t reserved_bytes;
    uint64_t highest_physical;
};

struct [[gnu::packed]] BlockDeviceInfo {
    uint64_t sector_size;
    uint64_t sector_count;
    uint64_t sector_reads;
    uint64_t cache_hits;
    uint64_t cache_misses;
    uint64_t cache_evictions;
    uint64_t invalid_reads;
    uint64_t null_buffer_rejects;
    uint64_t zero_count_rejects;
    uint64_t oversized_request_rejects;
    uint64_t backend_read_failures;
    uint64_t cache_fills;
    uint64_t cached_entries;
    uint64_t largest_request_sectors;
    uint64_t last_lba;
    uint32_t initialized;
    uint32_t reserved;
};

struct [[gnu::packed]] ProcessInfo {
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t state;
    uint32_t termination_reason;
    uint64_t entry;
    uint64_t address_space_root;
    uint64_t user_stack_top;
    uint64_t user_stack_pages;
    uint64_t image_base;
    uint64_t image_pages;
    uint64_t main_thread_id;
    uint64_t open_file_count;
    uint64_t owned_page_count;
    uint64_t exit_code;
    uint64_t process_group_id;
    uint64_t syscall_count;
    uint64_t last_syscall;
    uint64_t run_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
    char name[32];
};

struct [[gnu::packed]] UserThreadInfo {
    uint64_t tid;
    uint64_t pid;
    uint32_t state;
    uint32_t block_reason;
    uint64_t entry;
    uint64_t user_stack_pointer;
    uint64_t address_space_root;
    uint32_t wait_pipe_id;
    uint32_t reserved;
    uint64_t wait_process_id;
    uint64_t syscall_count;
    uint64_t last_syscall;
    uint64_t run_ticks;
    uint64_t switch_count;
    uint64_t preempt_count;
};

enum class FileDescriptorInfoKind : uint32_t {
    Empty = 0,
    Vfs = 1,
    PipeRead = 2,
    PipeWrite = 3,
};

struct [[gnu::packed]] FileDescriptorInfo {
    uint64_t pid;
    uint32_t fd;
    FileDescriptorInfoKind kind;
    uint32_t vfs_handle;
    uint32_t pipe_id;
    uint64_t offset;
    char path[64];
};

struct [[gnu::packed]] PipeInfo {
    uint32_t id;
    uint32_t open;
    uint64_t size;
    uint64_t capacity;
    uint64_t read_offset;
    uint64_t reader_count;
    uint64_t writer_count;
};

struct [[gnu::packed]] WaitAnyInfo {
    uint64_t pid;
    uint64_t exit_code;
};

struct [[gnu::packed]] CurrentIdsInfo {
    uint64_t pid;
    uint64_t tid;
    uint64_t parent_pid;
    uint64_t kernel_thread_id;
    uint64_t process_group_id;
    uint32_t cpu_id;
    uint32_t reserved;
};

struct [[gnu::packed]] SchedulerStatsInfo {
    uint64_t thread_count;
    uint64_t ready_count;
    uint64_t sleeping_count;
    uint64_t dead_count;
    uint64_t switch_count;
    uint64_t yield_count;
    uint64_t preempt_count;
    uint64_t current_thread_id;
    uint32_t current_cpu_id;
    uint32_t online_cpu_count;
};

struct [[gnu::packed]] CpuInfo {
    uint32_t cpu_id;
    uint32_t apic_id;
    uint32_t acpi_processor_id;
    uint32_t flags;
};

enum CpuInfoFlags : uint32_t {
    CpuInfoEnabled = 1u << 0,
    CpuInfoOnline = 1u << 1,
    CpuInfoBootstrap = 1u << 2,
    CpuInfoStartupAttempted = 1u << 3,
    CpuInfoParked = 1u << 4,
    CpuInfoScheduler = 1u << 5,
    CpuInfoDescriptorsReady = 1u << 6,
    CpuInfoLocalApicTimerReady = 1u << 7,
    CpuInfoBootstrapWorkDone = 1u << 8,
    CpuInfoIpiWorkDone = 1u << 9,
};

struct [[gnu::packed]] LaunchContextInfo {
    uint64_t tid;
    uint64_t pid;
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr3;
    uint16_t cs;
    uint16_t ss;
    uint32_t reserved;
    uint64_t rflags;
};

struct [[gnu::packed]] UserSchedulerInfo {
    uint64_t current_tid;
    uint64_t current_pid;
    uint64_t runnable_threads;
    uint64_t running_threads;
    uint64_t exited_threads;
    uint64_t last_selected_tid;
    uint64_t schedulable_threads;
    uint64_t timeslice_quantum;
    uint64_t current_slice_ticks;
    uint64_t expired_slices;
};

struct [[gnu::packed]] CurrentUserContextInfo {
    uint64_t pid;
    uint64_t tid;
    uint32_t process_state;
    uint32_t thread_state;
    uint64_t entry;
    uint64_t user_stack_pointer;
    uint64_t address_space_root;
};

enum class VfsNodeType : uint32_t {
    Empty = 0,
    Directory = 1,
    MemoryFile = 2,
    CharacterDevice = 3,
    VirtualFile = 4,
};

enum VfsNodeFlags : uint32_t {
    VfsNodeReadable = 1u << 0,
    VfsNodeWritable = 1u << 1,
    VfsNodeDirectory = 1u << 2,
    VfsNodeMemoryBacked = 1u << 3,
    VfsNodeDiskBacked = 1u << 4,
    VfsNodeCharacterDevice = 1u << 5,
    VfsNodeVirtual = 1u << 6,
};

enum MountFlags : uint32_t {
    MountReadOnly = 1u << 0,
    MountWritable = 1u << 1,
    MountMemoryBacked = 1u << 2,
    MountDiskBacked = 1u << 3,
};

struct [[gnu::packed]] VfsNodeInfo {
    VfsNodeType type;
    uint32_t flags;
    uint64_t base;
    uint64_t size;
    uint64_t links;
    char path[64];
};

struct [[gnu::packed]] VfsDirectoryEntryInfo {
    VfsNodeType type;
    uint32_t flags;
    uint64_t size;
    uint64_t links;
    char name[32];
    char path[64];
};

struct [[gnu::packed]] MountInfo {
    uint32_t flags;
    uint32_t reserved;
    uint64_t node_count;
    uint64_t total_bytes;
    char path[64];
    char fs_type[16];
    char source[32];
};

struct [[gnu::packed]] VfsStatInfo {
    VfsNodeType type;
    uint32_t flags;
    uint64_t size;
    uint64_t links;
    char path[64];
};

struct [[gnu::packed]] DateTimeInfo {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint8_t reserved;
};

struct [[gnu::packed]] PathInfo {
    char path[64];
};

struct [[gnu::packed]] ArgumentInfo {
    char value[64];
};

struct [[gnu::packed]] EnvironmentInfo {
    char key[24];
    char value[80];
};

struct [[gnu::packed]] SystemInfo {
    uint32_t boot_info_version;
    uint32_t boot_info_flags;
    uint64_t boot_module_count;
    uint64_t kernel_base;
    uint64_t kernel_end;
    uint64_t kernel_entry;
    uint64_t rsdp;
    char sysname[24];
    char release[24];
    char machine[16];
    char boot_mode[16];
    char kernel_type[32];
};

struct [[gnu::packed]] LimitsInfo {
    uint64_t max_boot_modules;
    uint64_t max_vfs_nodes;
    uint64_t max_file_handles;
    uint64_t max_ram_files;
    uint64_t max_ram_directories;
    uint64_t max_ram_links;
    uint64_t max_mounts;
    uint64_t max_ram_file_bytes;
    uint64_t max_process_file_descriptors;
    uint64_t max_owned_user_pages;
    uint64_t max_process_arguments;
    uint64_t max_argument_length;
    uint64_t max_environment_entries;
    uint64_t max_environment_key_length;
    uint64_t max_environment_value_length;
    uint64_t max_pipes;
    uint64_t pipe_capacity;
    uint64_t max_cpus;
    uint64_t pmm_bitmap_pages;
    uint64_t mounted_fat_path_capacity;
};

struct [[gnu::packed]] AbiInfo {
    uint32_t abi_version;
    uint32_t boot_info_version;
    uint64_t syscall_max_number;
    uint64_t syscall_result_size;
    uint64_t boot_info_size;
    uint64_t framebuffer_info_size;
    uint64_t memory_region_size;
    uint64_t boot_module_size;
    uint64_t system_info_size;
    uint64_t limits_info_size;
    uint64_t abi_info_size;
    uint64_t process_info_size;
    uint64_t user_thread_info_size;
    uint64_t vfs_node_info_size;
    uint64_t vfs_stat_info_size;
    uint64_t mount_info_size;
    uint64_t file_descriptor_info_size;
    uint64_t pipe_info_size;
    uint64_t block_device_info_size;
};

constexpr uint64_t kSyscallErrorNone = 0;
constexpr uint64_t kSyscallErrorInvalidSyscall = 1;
constexpr uint64_t kSyscallErrorInvalidPointer = 2;
constexpr uint64_t kSyscallErrorNotFound = 3;
constexpr uint64_t kSyscallErrorWouldBlock = 4;

} // namespace hybrid
