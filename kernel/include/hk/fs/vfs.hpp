#pragma once
#include <stddef.h>
#include <stdint.h>
#include "hybrid/boot_info.hpp"
#include "hybrid/syscall.hpp"

namespace hk::fs {

enum class NodeType : uint8_t { Empty, Directory, MemoryFile, CharacterDevice, VirtualFile };
enum class DeviceKind : uint8_t { None, Null, Zero, Tty, Console };
enum class VirtualFileKind : uint8_t {
    None,
    ProcMeminfo,
    ProcIomem,
    ProcRtc,
    ProcUptime,
    ProcLoadavg,
    ProcSchedDebug,
    ProcStat,
    ProcProcesses,
    ProcSelfStatus,
    ProcSelfStat,
    ProcSelfMaps,
    ProcSelfCmdline,
    ProcSelfEnviron,
    ProcSelfFd,
    ProcMounts,
    ProcFilesystems,
    ProcVfsStats,
    ProcBlockBootdisk,
    ProcDiskstats,
    ProcPartitions,
    ProcPciSummary,
    ProcPciDevices,
    ProcDevices,
    ProcDriverSummary,
    ProcDriverDevices,
    ProcIrqSummary,
    ProcInterrupts,
    ProcTtySummary,
    ProcCpuinfo,
    ProcCpuSummary,
    ProcCpuTopology,
    ProcMmSummary,
    ProcHeapinfo,
    ProcVmstat,
    ProcBuddyinfo,
    ProcKmsg,
    ProcNetSummary,
    ProcNetDev,
    ProcNetRoute,
    ProcModules,
    ProcBootinfo,
    ProcFeatures,
    ProcAbi,
    ProcCmdline,
    ProcHostname,
    ProcOstype,
    ProcOsrelease,
    ProcVersionString,
};

constexpr uint32_t kMaxVfsNodes = 512;
constexpr uint32_t kMaxFileHandles = 32;
constexpr uint32_t kMaxRamFiles = 32;
constexpr uint32_t kMaxRamDirectories = 8;
constexpr uint32_t kMaxRamLinks = 8;
constexpr uint32_t kMaxMounts = 8;
constexpr uint64_t kMaxRamFileBytes = 4096;

struct RamFile {
    bool used;
    char path[64];
    unsigned char data[kMaxRamFileBytes];
    uint64_t size;
};

struct RamDirectory {
    bool used;
    char path[64];
};

struct RamLink {
    bool used;
    char path[64];
    RamFile* target;
};

struct Node {
    const char* path;
    NodeType type;
    uint64_t base;
    uint64_t size;
    bool writable;
    bool disk_backed;
    RamFile* ram_file;
    DeviceKind device_kind;
    VirtualFileKind virtual_kind;
};

struct FileHandle {
    uint32_t id;
    const Node* node;
    uint64_t offset;
    uint32_t ref_count;
    bool open;
    bool dynamic_virtual;
    char path[64];
};

struct MountRecord {
    bool used;
    uint32_t flags;
    uint64_t node_count;
    uint64_t total_bytes;
    char path[64];
    char fs_type[16];
    char source[32];
};

struct VfsStats {
    uint64_t ram_file_creates;
    uint64_t ram_file_create_rejects;
    uint64_t ram_directory_creates;
    uint64_t ram_directory_create_rejects;
    uint64_t ram_links;
    uint64_t ram_link_rejects;
    uint64_t ram_truncates;
    uint64_t ram_truncate_rejects;
    uint64_t ram_renames;
    uint64_t ram_rename_rejects;
    uint64_t ram_file_deletes;
    uint64_t ram_file_delete_rejects;
    uint64_t ram_directory_deletes;
    uint64_t ram_directory_delete_rejects;
    uint64_t ram_write_bytes;
    uint64_t ram_write_clipped_bytes;
};

class Vfs {
public:
    void initialize(const hybrid::BootInfo& boot);
    bool register_directory(const char* path, bool writable = false);
    bool register_memory_file(const char* path, uint64_t base, uint64_t size);
    bool register_disk_file(const char* path, uint64_t base, uint64_t size);
    bool register_character_device(const char* path, DeviceKind kind);
    bool register_virtual_file(const char* path, VirtualFileKind kind);
    bool register_mount(const char* path, const char* fs_type, const char* source, uint32_t flags, uint64_t node_count, uint64_t total_bytes);
    bool create_ram_file(const char* path);
    bool link_ram_file(const char* existing_path, const char* new_path);
    bool truncate_ram_file(const char* path, uint64_t size);
    bool rename_ram_node(const char* old_path, const char* new_path);
    bool delete_ram_file(const char* path);
    bool create_ram_directory(const char* path);
    bool delete_ram_directory(const char* path);
    const Node* find(const char* path) const;
    size_t read(const char* path, uint64_t offset, void* buffer, size_t size) const;
    uint32_t open(const char* path);
    size_t read_handle(uint32_t handle, void* buffer, size_t size);
    size_t write_handle(uint32_t handle, const void* buffer, size_t size);
    bool seek_handle(uint32_t handle, uint64_t offset);
    bool retain_handle(uint32_t handle);
    uint64_t handle_offset(uint32_t handle) const;
    bool close(uint32_t handle);
    uint32_t open_handle_count() const;
    uint32_t node_count() const { return count_; }
    uint32_t mount_count() const;
    uint32_t memory_file_count() const;
    uint64_t total_memory_file_bytes() const;
    bool copy_node_info(uint32_t index, hybrid::VfsNodeInfo& out) const;
    bool copy_directory_entry(const char* path, uint32_t index, hybrid::VfsDirectoryEntryInfo& out) const;
    bool copy_mount_info(uint32_t index, hybrid::MountInfo& out) const;
    bool stat(const char* path, hybrid::VfsStatInfo& out) const;
    VfsStats stats() const { return stats_; }
private:
    Node nodes_[kMaxVfsNodes]{};
    FileHandle handles_[kMaxFileHandles]{};
    RamFile ram_files_[kMaxRamFiles]{};
    RamDirectory ram_directories_[kMaxRamDirectories]{};
    RamLink ram_links_[kMaxRamLinks]{};
    MountRecord mounts_[kMaxMounts]{};
    uint32_t count_ = 0;
    uint32_t next_handle_id_ = 1;
    VfsStats stats_{};
    bool add_node(const Node& node);
    FileHandle* handle_for(uint32_t handle);
    const FileHandle* handle_for(uint32_t handle) const;
};

Vfs& vfs();
uint32_t mounted_fat_path_capacity();
bool self_test();

} // namespace hk::fs
