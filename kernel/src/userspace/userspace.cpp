#include "hk/userspace/userspace.hpp"
#include "hk/cpu/gdt.hpp"
#include "hk/mm/address_space.hpp"
#include "hk/mm/pmm.hpp"
#include "hk/lib/string.hpp"
#include "hk/fs/vfs.hpp"
#include "hk/boot/bootinfo.hpp"
#include "hk/log.hpp"
#include "hk/timer/pit.hpp"

namespace hk::userspace {
namespace {
struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

constexpr uint32_t kElfLoad = 1;
constexpr uint32_t kElfExecute = 1;
constexpr uint32_t kElfWrite = 2;

bool valid_elf64(const Elf64_Ehdr* eh, uint64_t size) {
    if (size < sizeof(Elf64_Ehdr)) return false;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F') return false;
    if (eh->e_ident[4] != 2 || eh->e_ident[5] != 1) return false;
    if (eh->e_machine != 62 || eh->e_phentsize != sizeof(Elf64_Phdr)) return false;
    if (eh->e_phoff + static_cast<uint64_t>(eh->e_phnum) * sizeof(Elf64_Phdr) > size) return false;
    return true;
}

bool track_page(OwnedPage* pages, uint64_t& count, uint64_t virt, uint64_t phys) {
    if (count >= kMaxOwnedUserPages) return false;
    pages[count++] = OwnedPage{virt, phys};
    return true;
}

void copy_owned_pages(Process& process, const OwnedPage* pages, uint64_t count) {
    process.owned_page_count = count;
    for (uint64_t i = 0; i < count; ++i) process.owned_pages[i] = pages[i];
}

void copy_name(char (&out)[32], const char* name) {
    uint64_t i = 0;
    if (name) {
        for (; i + 1 < sizeof(out) && name[i] != 0; ++i) out[i] = name[i];
    }
    out[i] = 0;
    for (++i; i < sizeof(out); ++i) out[i] = 0;
}

template <uint64_t N>
bool copy_path_bounded(char (&out)[N], const char* path) {
    if (!path || path[0] != '/') return false;
    uint64_t i = 0;
    for (; i + 1 < N && path[i] != 0; ++i) out[i] = path[i];
    if (path[i] != 0) return false;
    out[i] = 0;
    for (++i; i < N; ++i) out[i] = 0;
    return true;
}

template <uint64_t N>
bool normalize_absolute_path(char (&out)[N], const char* path) {
    if (!path || path[0] != '/' || N < 2) return false;
    for (uint64_t i = 0; i < N; ++i) out[i] = 0;
    out[0] = '/';
    uint64_t out_len = 1;
    uint64_t cursor = 1;
    while (path[cursor] != 0) {
        while (path[cursor] == '/') ++cursor;
        if (path[cursor] == 0) break;
        uint64_t start = cursor;
        while (path[cursor] != 0 && path[cursor] != '/') ++cursor;
        uint64_t length = cursor - start;
        if (length == 1 && path[start] == '.') continue;
        if (length == 2 && path[start] == '.' && path[start + 1] == '.') {
            if (out_len > 1) {
                while (out_len > 1 && out[out_len - 1] != '/') --out_len;
                if (out_len > 1 && out[out_len - 1] == '/') --out_len;
            }
            out[out_len] = 0;
            continue;
        }
        if (out_len != 1) {
            if (out_len + 1 >= N) return false;
            out[out_len++] = '/';
        }
        if (out_len + length >= N) return false;
        for (uint64_t i = 0; i < length; ++i) out[out_len++] = path[start + i];
        out[out_len] = 0;
    }
    if (out_len == 0) {
        out[0] = '/';
        out_len = 1;
    }
    out[out_len] = 0;
    for (uint64_t i = out_len + 1; i < N; ++i) out[i] = 0;
    return true;
}

uint64_t string_length_bounded(const char* text, uint64_t max) {
    if (!text) return max + 1;
    uint64_t length = 0;
    while (length < max && text[length] != 0) ++length;
    return text[length] == 0 ? length : max + 1;
}

bool string_equal(const char* a, const char* b) {
    if (!a || !b) return false;
    uint64_t i = 0;
    while (a[i] != 0 && b[i] != 0) {
        if (a[i] != b[i]) return false;
        ++i;
    }
    return a[i] == 0 && b[i] == 0;
}

bool resolve_process_path(const Process& process, const char* path, char (&out)[64]) {
    if (!path || path[0] == 0) return false;
    if (path[0] == '/') return normalize_absolute_path(out, path);
    uint64_t cwd_len = string_length_bounded(process.current_directory, sizeof(process.current_directory) - 1);
    uint64_t path_len = string_length_bounded(path, sizeof(out) - 1);
    if (cwd_len == 0 || cwd_len >= sizeof(out) || path_len == 0 || path_len >= sizeof(out)) return false;
    char combined[64]{};
    uint64_t index = 0;
    for (; index < cwd_len; ++index) combined[index] = process.current_directory[index];
    if (cwd_len != 1 || process.current_directory[0] != '/') {
        if (index + 1 >= sizeof(out)) return false;
        combined[index++] = '/';
    }
    for (uint64_t i = 0; i < path_len; ++i) {
        if (index + 1 >= sizeof(out)) return false;
        combined[index++] = path[i];
    }
    combined[index] = 0;
    return normalize_absolute_path(out, combined);
}

bool map_kernel_identity_window(hk::mm::AddressSpace& space, uint64_t required_limit) {
    constexpr uint64_t kMinimumIdentityLimit = 256ull * 1024ull * 1024ull;
    uint64_t limit = required_limit < kMinimumIdentityLimit ? kMinimumIdentityLimit : required_limit;
    limit = hk::mm::align_up(limit);
    for (uint64_t phys = 0; phys < limit; phys += hk::mm::kPageSize) {
        if (hk::mm::translate(space, phys) != 0) continue;
        auto mapped = hk::mm::map_page(space, phys, phys, hk::mm::PageWrite);
        if (!mapped.ok) return false;
    }
    return true;
}

bool map_supervisor_range(hk::mm::AddressSpace& space, uint64_t base, uint64_t length, uint64_t flags) {
    if (base == 0 || length == 0) return true;
    uint64_t start = hk::mm::align_down(base);
    uint64_t end = hk::mm::align_up(base + length);
    for (uint64_t virt = start; virt < end; virt += hk::mm::kPageSize) {
        if (hk::mm::translate(space, virt) != 0) continue;
        auto mapped = hk::mm::map_page(space, virt, virt, flags);
        if (!mapped.ok) return false;
    }
    return true;
}

bool map_kernel_runtime_ranges(hk::mm::AddressSpace& space, uint64_t required_identity_limit) {
    if (!map_kernel_identity_window(space, required_identity_limit)) return false;
    const auto& fb = hk::boot::framebuffer_info();
    uint64_t fb_length = static_cast<uint64_t>(fb.pixels_per_scanline) * fb.height * fb.bytes_per_pixel;
    return map_supervisor_range(space, fb.base, fb_length, hk::mm::PageWrite | hk::mm::PageCacheDisable | hk::mm::PageWriteThrough);
}

void set_default_directory(Process& process) {
    process.current_directory[0] = '/';
    process.current_directory[1] = 0;
    for (uint64_t i = 2; i < sizeof(process.current_directory); ++i) process.current_directory[i] = 0;
}

bool copy_argument_bounded(char (&out)[kMaxArgumentLength], const char* value) {
    if (!value || value[0] == 0) return false;
    uint64_t i = 0;
    for (; i + 1 < sizeof(out) && value[i] != 0; ++i) out[i] = value[i];
    if (value[i] != 0) return false;
    out[i] = 0;
    for (++i; i < sizeof(out); ++i) out[i] = 0;
    return true;
}

template <uint64_t N>
bool copy_text_bounded(char (&out)[N], const char* value) {
    if (!value || value[0] == 0) return false;
    uint64_t i = 0;
    for (; i + 1 < N && value[i] != 0; ++i) out[i] = value[i];
    if (value[i] != 0) return false;
    out[i] = 0;
    for (++i; i < N; ++i) out[i] = 0;
    return true;
}

bool valid_environment_key(const char* key) {
    if (!key || key[0] == 0) return false;
    for (uint64_t i = 0; key[i] != 0; ++i) {
        char c = key[i];
        if (c == '=') return false;
        bool alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        bool digit = c >= '0' && c <= '9';
        if (!alpha && !digit && c != '_') return false;
    }
    return true;
}

void clear_arguments(Process& process) {
    process.argument_count = 0;
    for (uint32_t i = 0; i < kMaxProcessArguments; ++i) {
        for (uint32_t j = 0; j < kMaxArgumentLength; ++j) process.arguments[i][j] = 0;
    }
}

void clear_environment(Process& process) {
    process.environment_count = 0;
    for (uint32_t i = 0; i < kMaxEnvironmentEntries; ++i) {
        for (uint32_t j = 0; j < kMaxEnvironmentKeyLength; ++j) process.environment_keys[i][j] = 0;
        for (uint32_t j = 0; j < kMaxEnvironmentValueLength; ++j) process.environment_values[i][j] = 0;
    }
}

void close_descriptor(FileDescriptor& fd) {
    if (!fd.open) return;
    if (fd.kind == FileDescriptorKind::Vfs && fd.vfs_handle != 0) {
        hk::fs::vfs().close(fd.vfs_handle);
    }
    fd = FileDescriptor{};
}

void close_process_descriptor(Process& process, FileDescriptor& fd) {
    (void)process;
    if (!fd.open) return;
    if (fd.kind == FileDescriptorKind::Vfs && fd.vfs_handle != 0) {
        hk::fs::vfs().close(fd.vfs_handle);
    }
    fd = FileDescriptor{};
}

void sync_descriptor_offsets(Process& process, uint32_t handle, uint64_t offset) {
    if (handle == 0) return;
    for (auto& fd : process.file_descriptors) {
        if (fd.open && fd.kind == FileDescriptorKind::Vfs && fd.vfs_handle == handle) fd.offset = offset;
    }
}

void copy_descriptor_path(FileDescriptor& fd, const char* path) {
    for (uint64_t i = 0; i < sizeof(fd.path); ++i) fd.path[i] = 0;
    if (!path) return;
    uint64_t i = 0;
    for (; i + 1 < sizeof(fd.path) && path[i] != 0; ++i) fd.path[i] = path[i];
    fd.path[i] = 0;
}

FileDescriptor make_vfs_descriptor(uint32_t fd_number, uint32_t handle, const char* path, uint64_t offset) {
    FileDescriptor fd{};
    fd.fd = fd_number;
    fd.vfs_handle = handle;
    fd.pipe_id = 0;
    fd.offset = offset;
    fd.kind = FileDescriptorKind::Vfs;
    fd.open = true;
    copy_descriptor_path(fd, path);
    return fd;
}

FileDescriptor make_pipe_descriptor(uint32_t fd_number, uint32_t pipe_id, bool write_end) {
    FileDescriptor fd{};
    fd.fd = fd_number;
    fd.vfs_handle = 0;
    fd.pipe_id = pipe_id;
    fd.offset = 0;
    fd.kind = write_end ? FileDescriptorKind::PipeWrite : FileDescriptorKind::PipeRead;
    fd.open = true;
    return fd;
}

bool install_vfs_descriptor(Process& process, uint32_t fd_number, const char* path) {
    uint32_t handle = hk::fs::vfs().open(path);
    if (handle == 0) return false;
    for (auto& fd : process.file_descriptors) {
        if (fd.open && fd.fd == fd_number) close_process_descriptor(process, fd);
    }
    for (auto& fd : process.file_descriptors) {
        if (!fd.open) {
            fd = make_vfs_descriptor(fd_number, handle, path, 0);
            return true;
        }
    }
    hk::fs::vfs().close(handle);
    return false;
}

bool install_default_standard_fds(Process& process) {
    return install_vfs_descriptor(process, hybrid::kStdinFd, "/dev/tty") &&
           install_vfs_descriptor(process, hybrid::kStdoutFd, "/dev/tty") &&
           install_vfs_descriptor(process, hybrid::kStderrFd, "/dev/tty");
}
}

UserspaceManager& userspace_manager() {
    static UserspaceManager manager;
    return manager;
}

void UserspaceManager::initialize() {
    enabled_ = false;
    count_ = 0;
    thread_count_ = 0;
    next_pid_ = 1;
    next_tid_ = 1;
    active_pid_ = 0;
    active_tid_ = 0;
    last_selected_tid_ = 0;
    last_user_pick_index_ = 0;
    current_slice_ticks_ = 0;
    expired_slices_ = 0;
    foreground_process_group_id_ = 0;
    diagnostics_ = {};
    for (auto& pipe : pipes_) pipe = Pipe{};
}

Process* UserspaceManager::allocate_process_slot() {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].state == ProcessState::Empty) return &processes_[i];
    }
    if (count_ >= kMaxUserProcesses) return nullptr;
    return &processes_[count_++];
}

UserThread* UserspaceManager::allocate_thread_slot() {
    for (uint64_t i = 0; i < thread_count_; ++i) {
        if (threads_[i].state == UserThreadState::Empty) return &threads_[i];
    }
    if (thread_count_ >= kMaxUserThreads) return nullptr;
    return &threads_[thread_count_++];
}

Process* UserspaceManager::create_process(const char* name, uint64_t entry) {
    if (entry == 0) return nullptr;
    auto space = hk::mm::create_address_space();
    if (space.pml4 == 0) return nullptr;
    OwnedPage owned[kMaxOwnedUserPages]{};
    uint64_t owned_count = 0;

    for (uint64_t i = 0; i < kDefaultUserStackPages; ++i) {
        uint64_t phys = hk::mm::pmm().allocate_page();
        if (phys == 0) return nullptr;
        uint64_t virt = kDefaultUserStackTop - ((i + 1) * hk::mm::kPageSize);
        auto mapped = hk::mm::map_page(space, virt, phys, hk::mm::PageWrite | hk::mm::PageUser);
        if (!mapped.ok) return nullptr;
        if (!track_page(owned, owned_count, virt, phys)) return nullptr;
    }

    if (!map_kernel_runtime_ranges(space, entry + hk::mm::kPageSize)) return nullptr;

    Process* process = allocate_process_slot();
    if (!process) return nullptr;
    *process = Process{next_pid_++, 0, ProcessState::Created, name, entry, space.pml4, kDefaultUserStackTop, kDefaultUserStackPages, 0, 0, 0, 0, hybrid::ProcessTerminationReason::None, 3, {}, 0, {}, {}, 0, {}, 0, {}, {}, true, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    process->process_group_id = process->pid;
    set_default_directory(*process);
    copy_owned_pages(*process, owned, owned_count);
    if (!create_main_thread(*process)) return nullptr;
    if (!install_default_standard_fds(*process)) return nullptr;
    return process;
}

Process* UserspaceManager::create_process_from_elf(const char* name, uint64_t image_base, uint64_t image_size) {
    if (image_base == 0) return nullptr;
    auto* eh = reinterpret_cast<const Elf64_Ehdr*>(image_base);
    if (!valid_elf64(eh, image_size) || eh->e_entry == 0) return nullptr;

    auto space = hk::mm::create_address_space();
    if (space.pml4 == 0) return nullptr;

    uint64_t first_vaddr = UINT64_MAX;
    uint64_t last_vaddr = 0;
    uint64_t loaded_pages = 0;
    OwnedPage owned[kMaxOwnedUserPages]{};
    uint64_t owned_count = 0;
    auto* ph = reinterpret_cast<const Elf64_Phdr*>(image_base + eh->e_phoff);
    for (uint16_t i = 0; i < eh->e_phnum; ++i) {
        if (ph[i].p_type != kElfLoad) continue;
        if (ph[i].p_memsz < ph[i].p_filesz) return nullptr;
        if (ph[i].p_offset + ph[i].p_filesz > image_size) return nullptr;
        uint64_t seg_start = hk::mm::align_down(ph[i].p_vaddr);
        uint64_t seg_end = hk::mm::align_up(ph[i].p_vaddr + ph[i].p_memsz);
        if (seg_start < first_vaddr) first_vaddr = seg_start;
        if (seg_end > last_vaddr) last_vaddr = seg_end;
        for (uint64_t virt = seg_start; virt < seg_end; virt += hk::mm::kPageSize) {
            uint64_t phys = hk::mm::pmm().allocate_page();
            if (phys == 0) return nullptr;
            memset(reinterpret_cast<void*>(phys), 0, hk::mm::kPageSize);
            uint64_t copy_start = virt > ph[i].p_vaddr ? virt : ph[i].p_vaddr;
            uint64_t file_end = ph[i].p_vaddr + ph[i].p_filesz;
            uint64_t copy_end = (virt + hk::mm::kPageSize) < file_end ? (virt + hk::mm::kPageSize) : file_end;
            if (copy_end > copy_start) {
                auto* dst = reinterpret_cast<void*>(phys + (copy_start - virt));
                auto* src = reinterpret_cast<const void*>(image_base + ph[i].p_offset + (copy_start - ph[i].p_vaddr));
                memcpy(dst, src, copy_end - copy_start);
            }
            uint64_t page_flags = hk::mm::PageUser;
            if (ph[i].p_flags & kElfWrite) page_flags |= hk::mm::PageWrite;
            if ((ph[i].p_flags & kElfExecute) == 0) page_flags |= hk::mm::PageNoExecute;
            auto mapped = hk::mm::map_page(space, virt, phys, page_flags);
            if (!mapped.ok) return nullptr;
            if (!track_page(owned, owned_count, virt, phys)) return nullptr;
            ++loaded_pages;
        }
    }
    if (loaded_pages == 0 || eh->e_entry < first_vaddr || eh->e_entry >= last_vaddr) return nullptr;

    for (uint64_t i = 0; i < kDefaultUserStackPages; ++i) {
        uint64_t phys = hk::mm::pmm().allocate_page();
        if (phys == 0) return nullptr;
        memset(reinterpret_cast<void*>(phys), 0, hk::mm::kPageSize);
        uint64_t virt = kDefaultUserStackTop - ((i + 1) * hk::mm::kPageSize);
        auto mapped = hk::mm::map_page(space, virt, phys, hk::mm::PageWrite | hk::mm::PageUser);
        if (!mapped.ok) return nullptr;
        if (!track_page(owned, owned_count, virt, phys)) return nullptr;
    }

    if (!map_kernel_runtime_ranges(space, image_base + image_size)) return nullptr;

    Process* process = allocate_process_slot();
    if (!process) return nullptr;
    *process = Process{next_pid_++, 0, ProcessState::Created, name, eh->e_entry, space.pml4, kDefaultUserStackTop, kDefaultUserStackPages, first_vaddr, loaded_pages, 0, 0, hybrid::ProcessTerminationReason::None, 3, {}, 0, {}, {}, 0, {}, 0, {}, {}, true, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    process->process_group_id = process->pid;
    set_default_directory(*process);
    copy_owned_pages(*process, owned, owned_count);
    if (!create_main_thread(*process)) return nullptr;
    if (!install_default_standard_fds(*process)) return nullptr;
    return process;
}

Process* UserspaceManager::create_process_stub(const char* name, uint64_t entry, uint64_t address_space_root) {
    if (entry == 0) return nullptr;
    Process* process = allocate_process_slot();
    if (!process) return nullptr;
    *process = Process{next_pid_++, 0, ProcessState::Created, name, entry, address_space_root, 0, 0, 0, 0, 0, 0, hybrid::ProcessTerminationReason::None, 3, {}, 0, {}, {}, 0, {}, 0, {}, {}, false, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    process->process_group_id = process->pid;
    set_default_directory(*process);
    if (!install_default_standard_fds(*process)) return nullptr;
    return process;
}

UserThread* UserspaceManager::create_main_thread(Process& process) {
    if (process.entry == 0 || process.address_space_root == 0 || process.user_stack_top == 0) return nullptr;
    UserThread* thread = allocate_thread_slot();
    if (!thread) return nullptr;
    *thread = UserThread{};
    thread->tid = next_tid_++;
    thread->pid = process.pid;
    thread->state = UserThreadState::Created;
    thread->block_reason = UserBlockReason::None;
    thread->wait_pipe_id = 0;
    thread->wait_process_id = 0;
    thread->wait_wake_tick = 0;
    thread->wait_fd = 0;
    thread->wait_buffer = 0;
    thread->wait_size = 0;
    thread->entry = process.entry;
    thread->user_stack_pointer = process.user_stack_top - 16;
    thread->address_space_root = process.address_space_root;
    thread->rflags = 0x202;
    process.main_thread_id = thread->tid;
    return thread;
}

Process* UserspaceManager::find_process(uint64_t pid) {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty) return &processes_[i];
    }
    return nullptr;
}

UserThread* UserspaceManager::find_thread(uint64_t tid) {
    for (uint64_t i = 0; i < thread_count_; ++i) {
        if (threads_[i].tid == tid && threads_[i].state != UserThreadState::Empty) return &threads_[i];
    }
    return nullptr;
}

bool UserspaceManager::mark_runnable(uint64_t pid) {
    Process* process = find_process(pid);
    if (!process || process->state != ProcessState::Created) return false;
    process->state = ProcessState::Runnable;
    if (active_pid_ == 0) active_pid_ = process->pid;
    UserThread* thread = find_thread(process->main_thread_id);
    if (thread && thread->state == UserThreadState::Created) thread->state = UserThreadState::Runnable;
    return update_user_preemption_gate();
}

bool UserspaceManager::activate_thread(uint64_t tid) {
    UserThread* next = find_thread(tid);
    if (!next || (next->state != UserThreadState::Runnable && next->state != UserThreadState::Running)) return false;
    Process* process = find_process(next->pid);
    if (!process || process->state != ProcessState::Runnable) return false;
    if (active_tid_ != 0 && active_tid_ != tid) {
        UserThread* current = find_thread(active_tid_);
        if (current && current->state == UserThreadState::Running) current->state = UserThreadState::Runnable;
    }
    next->state = UserThreadState::Running;
    ++next->switch_count;
    ++process->switch_count;
    active_tid_ = next->tid;
    active_pid_ = next->pid;
    last_selected_tid_ = next->tid;
    current_slice_ticks_ = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        if (threads_[i].tid == tid) {
            last_user_pick_index_ = i;
            break;
        }
    }
    return true;
}

bool UserspaceManager::save_active_thread_frame(uint64_t rip, uint64_t rsp) {
    UserThread* thread = current_thread();
    if (!thread || thread->state == UserThreadState::Empty || thread->state == UserThreadState::Exited) return false;
    if (rip == 0 || rsp == 0) return false;
    Process* process = find_process(thread->pid);
    if (!process || process->state != ProcessState::Runnable) return false;
    hk::mm::AddressSpace space{thread->address_space_root};
    if (hk::mm::translate(space, rip) == 0 || hk::mm::translate(space, rsp) == 0) return false;
    thread->entry = rip;
    thread->user_stack_pointer = rsp;
    return true;
}

bool UserspaceManager::mark_active_thread_runnable() {
    UserThread* thread = current_thread();
    if (!thread || thread->state != UserThreadState::Running) return false;
    thread->state = UserThreadState::Runnable;
    thread->block_reason = UserBlockReason::None;
    thread->wait_pipe_id = 0;
    thread->wait_process_id = 0;
    thread->wait_wake_tick = 0;
    thread->wait_fd = 0;
    thread->wait_buffer = 0;
    thread->wait_size = 0;
    return true;
}

bool UserspaceManager::block_active_thread_on_pipe(uint32_t pipe_id, bool write_end, uint32_t fd, uint64_t buffer, uint64_t size) {
    UserThread* thread = current_thread();
    if (!thread || thread->state != UserThreadState::Running || pipe_id == 0 || !find_pipe(pipe_id)) return false;
    thread->state = UserThreadState::Blocked;
    thread->block_reason = write_end ? UserBlockReason::PipeWrite : UserBlockReason::PipeRead;
    thread->wait_pipe_id = pipe_id;
    thread->wait_process_id = 0;
    thread->wait_wake_tick = 0;
    thread->wait_fd = fd;
    thread->wait_buffer = buffer;
    thread->wait_size = size;
    if (write_end) {
        ++diagnostics_.pipe_write_blocks;
        hk::log_hex(hk::LogLevel::Info, "Userspace pipe write block count", diagnostics_.pipe_write_blocks);
    } else {
        ++diagnostics_.pipe_read_blocks;
        hk::log_hex(hk::LogLevel::Info, "Userspace pipe read block count", diagnostics_.pipe_read_blocks);
    }
    return true;
}

bool UserspaceManager::block_active_thread_on_process(uint64_t target_pid) {
    UserThread* thread = current_thread();
    if (!thread || thread->state != UserThreadState::Running || target_pid == 0) return false;
    Process* target = find_process(target_pid);
    if (!target || target->state == ProcessState::Empty || target->state == ProcessState::Exited) return false;
    thread->state = UserThreadState::Blocked;
    thread->block_reason = UserBlockReason::ProcessWait;
    thread->wait_pipe_id = 0;
    thread->wait_process_id = target_pid;
    thread->wait_wake_tick = 0;
    thread->wait_fd = 0;
    thread->wait_buffer = 0;
    thread->wait_size = 0;
    ++diagnostics_.process_wait_blocks;
    hk::log_hex(hk::LogLevel::Info, "Userspace wait block count", diagnostics_.process_wait_blocks);
    return true;
}

bool UserspaceManager::block_active_thread_on_any_process() {
    UserThread* thread = current_thread();
    if (!thread || thread->state != UserThreadState::Running || !process_wait_any_would_block(thread->pid)) return false;
    thread->state = UserThreadState::Blocked;
    thread->block_reason = UserBlockReason::ProcessWait;
    thread->wait_pipe_id = 0;
    thread->wait_process_id = 0;
    thread->wait_wake_tick = 0;
    thread->wait_fd = 0;
    thread->wait_buffer = 0;
    thread->wait_size = 0;
    ++diagnostics_.process_wait_any_blocks;
    hk::log_hex(hk::LogLevel::Info, "Userspace wait-any block count", diagnostics_.process_wait_any_blocks);
    return true;
}

bool UserspaceManager::block_active_thread_until(uint64_t wake_tick) {
    UserThread* thread = current_thread();
    if (!thread || thread->state != UserThreadState::Running || wake_tick == 0) return false;
    thread->state = UserThreadState::Blocked;
    thread->block_reason = UserBlockReason::Sleep;
    thread->wait_pipe_id = 0;
    thread->wait_process_id = 0;
    thread->wait_wake_tick = wake_tick;
    thread->wait_fd = 0;
    thread->wait_buffer = 0;
    thread->wait_size = 0;
    ++diagnostics_.sleep_blocks;
    hk::log_hex(hk::LogLevel::Info, "Userspace sleep block count", diagnostics_.sleep_blocks);
    return true;
}

uint64_t UserspaceManager::wake_pipe_waiters(uint32_t pipe_id, bool readers, bool writers) {
    if (pipe_id == 0) return 0;
    Pipe* pipe = find_pipe(pipe_id);
    uint64_t woken = 0;
    bool allow_readers = readers;
    bool allow_writers = writers;
    for (uint64_t round = 0; round < thread_count_ * 2 + 1; ++round) {
        bool progress = false;
        bool completed_read = false;
        bool completed_write = false;
        for (uint64_t i = 0; i < thread_count_; ++i) {
            UserThread& thread = threads_[i];
            if (thread.state != UserThreadState::Blocked || thread.wait_pipe_id != pipe_id) continue;
            bool wake = (allow_readers && thread.block_reason == UserBlockReason::PipeRead) ||
                (allow_writers && thread.block_reason == UserBlockReason::PipeWrite);
            if (!wake) continue;
            Process* process = find_process(thread.pid);
            if (!process || process->state != ProcessState::Runnable) continue;
            bool completed = false;
            bool write_result = false;
            bool broken_write = false;
            if (allow_readers && thread.block_reason == UserBlockReason::PipeRead) {
                if (pipe && thread.wait_buffer != 0 && thread.wait_size != 0 && pipe->read_offset < pipe->size) {
                    hk::mm::AddressSpace waiter_space{thread.address_space_root};
                    uint64_t dest_phys = hk::mm::translate(waiter_space, thread.wait_buffer);
                    if (dest_phys != 0) {
                        uint64_t available = pipe->size - pipe->read_offset;
                        uint64_t page_remaining = hk::mm::kPageSize - (dest_phys & (hk::mm::kPageSize - 1));
                        uint64_t to_copy = thread.wait_size < available ? thread.wait_size : available;
                        if (page_remaining < to_copy) to_copy = page_remaining;
                        if (to_copy != 0) {
                            memcpy(reinterpret_cast<void*>(dest_phys), pipe->data + pipe->read_offset, to_copy);
                            pipe->read_offset += to_copy;
                            if (pipe->read_offset != 0) {
                                uint64_t remaining = pipe->size - pipe->read_offset;
                                if (remaining != 0) memmove(pipe->data, pipe->data + pipe->read_offset, remaining);
                                pipe->size = remaining;
                                pipe->read_offset = 0;
                            }
                            thread.rax = to_copy;
                            thread.rdx = hybrid::kSyscallErrorNone;
                            completed = true;
                            completed_read = true;
                            hk::log_hex(hk::LogLevel::Info, "Userspace pipe read resumed bytes", to_copy);
                        }
                    }
                } else if (!pipe_has_live_writer(pipe_id)) {
                    thread.rax = 0;
                    thread.rdx = hybrid::kSyscallErrorNone;
                    completed = true;
                    completed_read = true;
                    hk::log_hex(hk::LogLevel::Info, "Userspace pipe read eof TID", thread.tid);
                }
            }
            if (allow_writers && thread.block_reason == UserBlockReason::PipeWrite) {
                if (!pipe_has_live_reader(pipe_id)) {
                    thread.rax = 0;
                    thread.rdx = hybrid::kSyscallErrorNotFound;
                    completed = true;
                    completed_write = true;
                    write_result = true;
                    broken_write = true;
                    hk::log_hex(hk::LogLevel::Info, "Userspace pipe write broken TID", thread.tid);
                } else if (pipe && thread.wait_buffer != 0 && thread.wait_size != 0 && pipe->size < kPipeCapacity) {
                    hk::mm::AddressSpace writer_space{thread.address_space_root};
                    uint64_t source_phys = hk::mm::translate(writer_space, thread.wait_buffer);
                    if (source_phys != 0) {
                        uint64_t available = kPipeCapacity - pipe->size;
                        uint64_t page_remaining = hk::mm::kPageSize - (source_phys & (hk::mm::kPageSize - 1));
                        uint64_t to_copy = thread.wait_size < available ? thread.wait_size : available;
                        if (page_remaining < to_copy) to_copy = page_remaining;
                        if (to_copy != 0) {
                            memcpy(pipe->data + pipe->size, reinterpret_cast<const void*>(source_phys), to_copy);
                            pipe->size += to_copy;
                            thread.rax = to_copy;
                            thread.rdx = hybrid::kSyscallErrorNone;
                            completed = true;
                            completed_write = true;
                            write_result = true;
                            hk::log_hex(hk::LogLevel::Info, "Userspace pipe write resumed bytes", to_copy);
                        }
                    }
                }
            }
            if (!completed) continue;
            thread.state = UserThreadState::Runnable;
            thread.block_reason = UserBlockReason::None;
            thread.wait_pipe_id = 0;
            thread.wait_process_id = 0;
            thread.wait_wake_tick = 0;
            thread.wait_fd = 0;
            thread.wait_buffer = 0;
            thread.wait_size = 0;
            if (!broken_write) hk::log_hex(hk::LogLevel::Info, write_result ? "Userspace pipe write resumed TID" : "Userspace pipe read resumed TID", thread.tid);
            if (write_result) {
                ++diagnostics_.pipe_write_wakes;
                hk::log_hex(hk::LogLevel::Info, "Userspace pipe write wake count", diagnostics_.pipe_write_wakes);
            } else {
                ++diagnostics_.pipe_read_wakes;
                hk::log_hex(hk::LogLevel::Info, "Userspace pipe read wake count", diagnostics_.pipe_read_wakes);
            }
            ++woken;
            progress = true;
        }
        if (completed_read) allow_writers = true;
        if (completed_write) allow_readers = true;
        if (!progress) break;
    }
    return woken;
}

uint64_t UserspaceManager::wake_process_waiters(uint64_t target_pid, uint64_t exit_code) {
    if (target_pid == 0) return 0;
    uint64_t woken = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        UserThread& thread = threads_[i];
        if (thread.state != UserThreadState::Blocked ||
            thread.block_reason != UserBlockReason::ProcessWait) continue;
        Process* waiter = find_process(thread.pid);
        if (!waiter || waiter->state != ProcessState::Runnable) continue;
        Process* target = find_process(target_pid);
        if (!target || target->parent_pid != waiter->pid) continue;
        bool any_wait = thread.wait_process_id == 0;
        if (!any_wait && thread.wait_process_id != target_pid) continue;
        uint64_t result_pointer = any_wait ? thread.rdi : thread.rsi;
        uint64_t result_size = any_wait ? sizeof(hybrid::WaitAnyInfo) : sizeof(uint64_t);
        if (result_pointer == 0 || (result_pointer & (sizeof(uint64_t) - 1)) != 0) continue;
        hk::mm::AddressSpace waiter_space{thread.address_space_root};
        uint64_t result_phys = hk::mm::translate(waiter_space, result_pointer);
        if (result_phys == 0 || (result_phys & (hk::mm::kPageSize - 1)) > hk::mm::kPageSize - result_size) continue;
        if (any_wait) {
            auto* info = reinterpret_cast<hybrid::WaitAnyInfo*>(result_phys);
            info->pid = target_pid;
            info->exit_code = exit_code;
        } else {
            *reinterpret_cast<uint64_t*>(result_phys) = exit_code;
        }
        thread.rax = 1;
        thread.rdx = hybrid::kSyscallErrorNone;
        thread.state = UserThreadState::Runnable;
        thread.block_reason = UserBlockReason::None;
        thread.wait_pipe_id = 0;
        thread.wait_process_id = 0;
        thread.wait_wake_tick = 0;
        thread.wait_fd = 0;
        thread.wait_buffer = 0;
        thread.wait_size = 0;
        ++diagnostics_.process_wait_wakes;
        hk::log_hex(hk::LogLevel::Info, "Userspace wait wake count", diagnostics_.process_wait_wakes);
        ++woken;
    }
    return woken;
}

uint64_t UserspaceManager::wake_sleepers(uint64_t now_tick) {
    if (now_tick == 0) return 0;
    uint64_t woken = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        UserThread& thread = threads_[i];
        if (thread.state != UserThreadState::Blocked ||
            thread.block_reason != UserBlockReason::Sleep ||
            thread.wait_wake_tick == 0 ||
            thread.wait_wake_tick > now_tick) {
            continue;
        }
        Process* process = find_process(thread.pid);
        if (!process || process->state != ProcessState::Runnable) continue;
        thread.rax = 0;
        thread.rdx = hybrid::kSyscallErrorNone;
        thread.state = UserThreadState::Runnable;
        thread.block_reason = UserBlockReason::None;
        thread.wait_pipe_id = 0;
        thread.wait_process_id = 0;
        thread.wait_wake_tick = 0;
        thread.wait_fd = 0;
        thread.wait_buffer = 0;
        thread.wait_size = 0;
        ++diagnostics_.sleep_wakes;
        hk::log_hex(hk::LogLevel::Info, "Userspace sleep wake count", diagnostics_.sleep_wakes);
        ++woken;
    }
    return woken;
}

bool UserspaceManager::save_current_context(UserExecutionContext& out) const {
    out = UserExecutionContext{};
    uint64_t tid = current_tid();
    uint64_t pid = current_pid();
    if (tid == 0 || pid == 0) return false;
    const UserThread* thread = nullptr;
    const Process* process = nullptr;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        if (threads_[i].tid == tid && threads_[i].state != UserThreadState::Empty && threads_[i].pid == pid) {
            thread = &threads_[i];
            break;
        }
    }
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state == ProcessState::Runnable) {
            process = &processes_[i];
            break;
        }
    }
    if (!thread || !process || thread->address_space_root == 0) return false;
    out.tid = thread->tid;
    out.pid = process->pid;
    out.cr3 = thread->address_space_root;
    out.valid = true;
    return true;
}

bool UserspaceManager::restore_context(const UserExecutionContext& context) {
    if (!context.valid || context.tid == 0 || context.pid == 0 || context.cr3 == 0) return false;
    UserThread* thread = find_thread(context.tid);
    Process* process = find_process(context.pid);
    if (!thread || !process || thread->pid != process->pid || thread->address_space_root != context.cr3) return false;
    if (process->state != ProcessState::Runnable) return false;
    if (thread->state == UserThreadState::Exited || thread->state == UserThreadState::Empty) return false;
    return activate_thread(context.tid);
}

Process* UserspaceManager::current_process() {
    uint64_t pid = current_pid();
    return pid == 0 ? nullptr : find_process(pid);
}

UserThread* UserspaceManager::current_thread() {
    uint64_t tid = current_tid();
    return tid == 0 ? nullptr : find_thread(tid);
}

uint64_t UserspaceManager::current_tid() const {
    if (active_tid_ == 0) return 0;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        const UserThread& thread = threads_[i];
        if (thread.tid == active_tid_ && thread.state != UserThreadState::Empty && thread.state != UserThreadState::Exited) {
            return active_tid_;
        }
    }
    return 0;
}

uint64_t UserspaceManager::current_pid() const {
    uint64_t tid = current_tid();
    if (tid != 0) {
        for (uint64_t i = 0; i < thread_count_; ++i) {
            const UserThread& thread = threads_[i];
            if (thread.tid != tid) continue;
            for (uint64_t j = 0; j < count_; ++j) {
                const Process& process = processes_[j];
                if (process.pid == thread.pid && process.state != ProcessState::Empty && process.state != ProcessState::Exited) {
                    return thread.pid;
                }
            }
            return 0;
        }
    }
    if (active_pid_ == 0) return 0;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.pid == active_pid_ && process.state != ProcessState::Empty && process.state != ProcessState::Exited) {
            return active_pid_;
        }
    }
    return 0;
}

uint64_t UserspaceManager::active_tid() const {
    return current_tid();
}

uint64_t UserspaceManager::active_pid() const {
    return current_pid();
}

bool UserspaceManager::set_parent(uint64_t pid, uint64_t parent_pid) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (parent_pid != 0 && !find_process(parent_pid)) return false;
    process->parent_pid = parent_pid;
    return true;
}

bool UserspaceManager::set_process_group(uint64_t caller_pid, uint64_t pid, uint64_t process_group_id) {
    if (caller_pid == 0 || pid == 0 || process_group_id == 0) return false;
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (process->pid != caller_pid && process->parent_pid != caller_pid) return false;
    process->process_group_id = process_group_id;
    return true;
}

bool UserspaceManager::set_foreground_process_group(uint64_t caller_pid, uint64_t process_group_id) {
    if (caller_pid == 0) return false;
    Process* caller = find_process(caller_pid);
    if (!caller || caller->state == ProcessState::Empty || caller->state == ProcessState::Exited) return false;
    uint64_t target_group = process_group_id == 0 ? caller->process_group_id : process_group_id;
    if (target_group == 0) return false;
    if (target_group == caller->process_group_id) {
        foreground_process_group_id_ = target_group;
        return true;
    }
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        if (process.parent_pid == caller_pid && process.process_group_id == target_group) {
            foreground_process_group_id_ = target_group;
            return true;
        }
    }
    return false;
}

uint64_t UserspaceManager::kill_process_group(uint64_t caller_pid, uint64_t process_group_id, uint64_t code, hybrid::ProcessTerminationReason reason) {
    if (caller_pid == 0 || process_group_id == 0) return 0;
    Process* caller = find_process(caller_pid);
    if (!caller || caller->state == ProcessState::Empty || caller->state == ProcessState::Exited) return 0;
    uint64_t killed = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        if (process.process_group_id != process_group_id || process.pid == caller_pid) continue;
        bool direct_child = process.parent_pid == caller_pid;
        bool same_parent = caller->parent_pid != 0 && process.parent_pid == caller->parent_pid;
        if (!direct_child && !same_parent) continue;
        if (exit_process(process.pid, code, reason)) ++killed;
    }
    return killed;
}

uint64_t UserspaceManager::stop_process_group(uint64_t caller_pid, uint64_t process_group_id) {
    if (caller_pid == 0 || process_group_id == 0) return 0;
    uint64_t stopped = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        if (process.process_group_id != process_group_id || process.pid == caller_pid) continue;
        if (process.parent_pid != caller_pid) continue;
        if (process.state == ProcessState::Runnable) {
            process.state = ProcessState::Stopped;
            UserThread* thread = find_thread(process.main_thread_id);
            if (thread && thread->state == UserThreadState::Running) thread->state = UserThreadState::Runnable;
            if (active_pid_ == process.pid) active_pid_ = 0;
            if (active_tid_ == process.main_thread_id) active_tid_ = 0;
            ++stopped;
        }
    }
    return stopped;
}

uint64_t UserspaceManager::continue_process_group(uint64_t caller_pid, uint64_t process_group_id) {
    if (caller_pid == 0 || process_group_id == 0) return 0;
    uint64_t continued = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        if (process.process_group_id != process_group_id || process.pid == caller_pid) continue;
        if (process.parent_pid != caller_pid) continue;
        if (process.state == ProcessState::Stopped) {
            process.state = ProcessState::Runnable;
            ++continued;
        }
    }
    if (continued != 0 && !update_user_preemption_gate()) return 0;
    return continued;
}

bool UserspaceManager::exit_process(uint64_t pid, uint64_t code, hybrid::ProcessTerminationReason reason) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Exited || process->state == ProcessState::Empty) return false;
    uint32_t reader_pipes[kMaxProcessFileDescriptors]{};
    uint32_t writer_pipes[kMaxProcessFileDescriptors]{};
    uint32_t reader_pipe_count = 0;
    uint32_t writer_pipe_count = 0;
    for (const auto& fd : process->file_descriptors) {
        if (!fd.open || fd.pipe_id == 0) continue;
        if (fd.kind == FileDescriptorKind::PipeRead && reader_pipe_count < kMaxProcessFileDescriptors) {
            reader_pipes[reader_pipe_count++] = fd.pipe_id;
        } else if (fd.kind == FileDescriptorKind::PipeWrite && writer_pipe_count < kMaxProcessFileDescriptors) {
            writer_pipes[writer_pipe_count++] = fd.pipe_id;
        }
    }
    process->state = ProcessState::Exited;
    process->exit_code = code;
    process->termination_reason = reason;
    UserThread* thread = find_thread(process->main_thread_id);
    if (thread && thread->state != UserThreadState::Exited) thread->state = UserThreadState::Exited;
    for (uint32_t i = 0; i < reader_pipe_count; ++i) wake_pipe_waiters(reader_pipes[i], false, true);
    for (uint32_t i = 0; i < writer_pipe_count; ++i) wake_pipe_waiters(writer_pipes[i], true, false);
    wake_process_waiters(process->pid, process->exit_code);
    if (active_tid_ == process->main_thread_id) active_tid_ = 0;
    if (active_pid_ == process->pid) active_pid_ = 0;
    return update_user_preemption_gate();
}

bool UserspaceManager::wait_process(uint64_t waiter_pid, uint64_t target_pid, uint64_t& exit_code) const {
    exit_code = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.pid != target_pid || process.state == ProcessState::Empty) continue;
        if (process.parent_pid != waiter_pid) return false;
        if (process.state != ProcessState::Exited) return false;
        exit_code = process.exit_code;
        return true;
    }
    return false;
}

bool UserspaceManager::wait_any_process(uint64_t waiter_pid, hybrid::WaitAnyInfo& out) const {
    out = hybrid::WaitAnyInfo{};
    if (waiter_pid == 0) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.parent_pid != waiter_pid) continue;
        if (process.state != ProcessState::Exited) continue;
        out.pid = process.pid;
        out.exit_code = process.exit_code;
        return true;
    }
    return false;
}

bool UserspaceManager::process_wait_would_block(uint64_t waiter_pid, uint64_t target_pid) const {
    if (waiter_pid == 0 || target_pid == 0) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.pid != target_pid || process.state == ProcessState::Empty) continue;
        return process.parent_pid == waiter_pid && process.state != ProcessState::Exited;
    }
    return false;
}

bool UserspaceManager::process_wait_any_would_block(uint64_t waiter_pid) const {
    if (waiter_pid == 0) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.parent_pid != waiter_pid) continue;
        if (process.state != ProcessState::Exited) return true;
    }
    return false;
}

bool UserspaceManager::reap_exited(uint64_t pid) {
    Process* process = find_process(pid);
    if (!process || process->state != ProcessState::Exited) return false;
    uint32_t closed_pipes[kMaxProcessFileDescriptors]{};
    uint32_t closed_pipe_count = 0;
    for (auto& fd : process->file_descriptors) {
        if (fd.open && (fd.kind == FileDescriptorKind::PipeRead || fd.kind == FileDescriptorKind::PipeWrite) &&
            closed_pipe_count < kMaxProcessFileDescriptors) {
            closed_pipes[closed_pipe_count++] = fd.pipe_id;
        }
        close_process_descriptor(*process, fd);
    }
    for (uint32_t i = 0; i < closed_pipe_count; ++i) cleanup_pipe_if_unreferenced(closed_pipes[i]);
    for (uint64_t i = 0; i < process->owned_page_count; ++i) {
        hk::mm::pmm().free_page(process->owned_pages[i].phys);
        process->owned_pages[i] = OwnedPage{};
    }
    process->owned_page_count = 0;
    if (process->owns_address_space && process->address_space_root != 0) {
        hk::mm::AddressSpace space{process->address_space_root};
        hk::mm::destroy_address_space(space);
    }
    for (uint64_t i = 0; i < thread_count_; ++i) {
        UserThread& thread = threads_[i];
        if (thread.pid != process->pid || thread.state == UserThreadState::Empty) continue;
        if (active_tid_ == thread.tid) active_tid_ = 0;
        if (last_selected_tid_ == thread.tid) last_selected_tid_ = 0;
        thread = UserThread{};
    }
    if (active_pid_ == process->pid) active_pid_ = 0;
    process->state = ProcessState::Empty;
    process->name = nullptr;
    process->parent_pid = 0;
    process->process_group_id = 0;
    process->entry = 0;
    process->address_space_root = 0;
    process->user_stack_top = 0;
    process->user_stack_pages = 0;
    process->image_base = 0;
    process->image_pages = 0;
    process->main_thread_id = 0;
    process->next_fd = 3;
    process->termination_reason = hybrid::ProcessTerminationReason::None;
    for (uint64_t i = 0; i < sizeof(process->current_directory); ++i) process->current_directory[i] = 0;
    clear_arguments(*process);
    clear_environment(*process);
    process->owns_address_space = false;
    return update_user_preemption_gate();
}

uint32_t UserspaceManager::open_file(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return 0;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return 0;
    uint32_t handle = hk::fs::vfs().open(resolved);
    if (handle == 0) return 0;
    for (auto& fd : process->file_descriptors) {
        if (!fd.open) {
            uint32_t next_fd = process->next_fd++;
            if (next_fd < 3) next_fd = process->next_fd++;
            fd = make_vfs_descriptor(next_fd, handle, resolved, 0);
            return next_fd;
        }
    }
    hk::fs::vfs().close(handle);
    return 0;
}

bool UserspaceManager::redirect_file(uint64_t pid, uint32_t target_fd, const char* path) {
    if (target_fd != hybrid::kStdinFd && target_fd != hybrid::kStdoutFd && target_fd != hybrid::kStderrFd) return false;
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    uint32_t handle = hk::fs::vfs().open(resolved);
    if (handle == 0) return false;
    for (auto& fd : process->file_descriptors) {
        if (fd.open && fd.fd == target_fd) {
            close_process_descriptor(*process, fd);
        }
    }
    for (auto& fd : process->file_descriptors) {
        if (!fd.open) {
            fd = make_vfs_descriptor(target_fd, handle, resolved, 0);
            return true;
        }
    }
    hk::fs::vfs().close(handle);
    return false;
}

bool UserspaceManager::redirect_file_append(uint64_t pid, uint32_t target_fd, const char* path) {
    if (target_fd != hybrid::kStdoutFd && target_fd != hybrid::kStderrFd) return false;
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    const auto* node = hk::fs::vfs().find(resolved);
    uint64_t end_offset = node && node->ram_file ? node->ram_file->size : (node ? node->size : 0);
    if (!redirect_file(pid, target_fd, path)) return false;
    return seek_file(pid, target_fd, end_offset);
}

Pipe* UserspaceManager::find_pipe(uint32_t pipe_id) {
    if (pipe_id == 0) return nullptr;
    for (auto& pipe : pipes_) {
        if (pipe.open && pipe.id == pipe_id) return &pipe;
    }
    return nullptr;
}

const Pipe* UserspaceManager::find_pipe(uint32_t pipe_id) const {
    if (pipe_id == 0) return nullptr;
    for (const auto& pipe : pipes_) {
        if (pipe.open && pipe.id == pipe_id) return &pipe;
    }
    return nullptr;
}

bool UserspaceManager::pipe_has_live_writer(uint32_t pipe_id) const {
    if (pipe_id == 0) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        for (const auto& fd : process.file_descriptors) {
            if (fd.open && fd.pipe_id == pipe_id && fd.kind == FileDescriptorKind::PipeWrite) return true;
        }
    }
    return false;
}

bool UserspaceManager::pipe_has_live_reader(uint32_t pipe_id) const {
    if (pipe_id == 0) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        for (const auto& fd : process.file_descriptors) {
            if (fd.open && fd.pipe_id == pipe_id && fd.kind == FileDescriptorKind::PipeRead) return true;
        }
    }
    return false;
}

bool UserspaceManager::cleanup_pipe_if_unreferenced(uint32_t pipe_id) {
    Pipe* pipe = find_pipe(pipe_id);
    if (!pipe) return false;
    bool has_reader = pipe_has_live_reader(pipe_id);
    bool has_writer = pipe_has_live_writer(pipe_id);
    if (has_reader || has_writer) {
        if (!has_reader) wake_pipe_waiters(pipe_id, false, true);
        if (!has_writer) wake_pipe_waiters(pipe_id, true, false);
        return true;
    }
    wake_pipe_waiters(pipe_id, true, true);
    *pipe = Pipe{};
    return true;
}

uint32_t UserspaceManager::create_pipe() {
    static uint32_t next_pipe_id = 1;
    for (auto& pipe : pipes_) {
        if (pipe.open) continue;
        uint32_t id = next_pipe_id++;
        if (id == 0) id = next_pipe_id++;
        pipe = Pipe{id, true, 0, 0, {}};
        return id;
    }
    return 0;
}

bool UserspaceManager::attach_pipe_fd(uint64_t pid, uint32_t target_fd, uint32_t pipe_id, bool write_end) {
    if (write_end) {
        if (target_fd != hybrid::kStdoutFd && target_fd != hybrid::kStderrFd) return false;
    } else if (target_fd != hybrid::kStdinFd) {
        return false;
    }
    Process* process = find_process(pid);
    Pipe* pipe = find_pipe(pipe_id);
    if (!process || !pipe || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    uint32_t replaced_pipe = 0;
    for (auto& fd : process->file_descriptors) {
        if (fd.open && fd.fd == target_fd) {
            if (fd.kind == FileDescriptorKind::PipeRead || fd.kind == FileDescriptorKind::PipeWrite) replaced_pipe = fd.pipe_id;
            close_process_descriptor(*process, fd);
        }
    }
    if (replaced_pipe != 0) cleanup_pipe_if_unreferenced(replaced_pipe);
    for (auto& fd : process->file_descriptors) {
        if (!fd.open) {
            fd = make_pipe_descriptor(target_fd, pipe_id, write_end);
            return true;
        }
    }
    return false;
}

bool UserspaceManager::close_pipe(uint32_t pipe_id) {
    return cleanup_pipe_if_unreferenced(pipe_id);
}

bool UserspaceManager::has_open_file(uint64_t pid, uint32_t fd) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (const auto& entry : processes_[i].file_descriptors) {
            if (entry.open && entry.fd == fd) return true;
        }
        return false;
    }
    return false;
}

bool UserspaceManager::is_pipe_fd(uint64_t pid, uint32_t fd) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (const auto& entry : processes_[i].file_descriptors) {
            if (!entry.open || entry.fd != fd) continue;
            return entry.kind == FileDescriptorKind::PipeRead || entry.kind == FileDescriptorKind::PipeWrite;
        }
        return false;
    }
    return false;
}

bool UserspaceManager::pipe_read_would_block(uint64_t pid, uint32_t fd) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (const auto& entry : processes_[i].file_descriptors) {
            if (!entry.open || entry.fd != fd || entry.kind != FileDescriptorKind::PipeRead) continue;
            const Pipe* pipe = find_pipe(entry.pipe_id);
            return pipe && pipe->read_offset >= pipe->size && pipe_has_live_writer(entry.pipe_id);
        }
        return false;
    }
    return false;
}

bool UserspaceManager::pipe_write_would_block(uint64_t pid, uint32_t fd) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (const auto& entry : processes_[i].file_descriptors) {
            if (!entry.open || entry.fd != fd || entry.kind != FileDescriptorKind::PipeWrite) continue;
            const Pipe* pipe = find_pipe(entry.pipe_id);
            return pipe && pipe->size >= kPipeCapacity && pipe_has_live_reader(entry.pipe_id);
        }
        return false;
    }
    return false;
}

bool UserspaceManager::resolve_pipe_fd(uint64_t pid, uint32_t fd, bool write_end, uint32_t& pipe_id) const {
    pipe_id = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (const auto& entry : processes_[i].file_descriptors) {
            if (!entry.open || entry.fd != fd) continue;
            FileDescriptorKind expected = write_end ? FileDescriptorKind::PipeWrite : FileDescriptorKind::PipeRead;
            if (entry.kind != expected || !find_pipe(entry.pipe_id)) return false;
            pipe_id = entry.pipe_id;
            return true;
        }
        return false;
    }
    return false;
}

bool UserspaceManager::create_file(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().create_ram_file(resolved);
}

bool UserspaceManager::create_directory(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().create_ram_directory(resolved);
}

bool UserspaceManager::link_file(uint64_t pid, const char* existing_path, const char* new_path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char existing[64]{};
    char linked[64]{};
    if (!resolve_process_path(*process, existing_path, existing) ||
        !resolve_process_path(*process, new_path, linked)) {
        return false;
    }
    return hk::fs::vfs().link_ram_file(existing, linked);
}

bool UserspaceManager::truncate_file(uint64_t pid, const char* path, uint64_t size) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().truncate_ram_file(resolved, size);
}

bool UserspaceManager::rename_path(uint64_t pid, const char* old_path, const char* new_path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char old_resolved[64]{};
    char new_resolved[64]{};
    if (!resolve_process_path(*process, old_path, old_resolved) ||
        !resolve_process_path(*process, new_path, new_resolved)) {
        return false;
    }
    return hk::fs::vfs().rename_ram_node(old_resolved, new_resolved);
}

uint64_t UserspaceManager::read_file(uint64_t pid, uint32_t fd, void* buffer, uint64_t size) {
    Process* process = find_process(pid);
    if (!process || !buffer || size == 0 || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return 0;
    for (auto& entry : process->file_descriptors) {
        if (entry.open && entry.fd == fd) {
            if (entry.kind == FileDescriptorKind::Vfs) {
                uint64_t bytes = hk::fs::vfs().read_handle(entry.vfs_handle, buffer, static_cast<size_t>(size));
                sync_descriptor_offsets(*process, entry.vfs_handle, hk::fs::vfs().handle_offset(entry.vfs_handle));
                ++process->read_syscalls;
                process->read_bytes += bytes;
                return bytes;
            }
            if (entry.kind == FileDescriptorKind::PipeRead) {
                Pipe* pipe = find_pipe(entry.pipe_id);
                if (!pipe || pipe->read_offset >= pipe->size) return 0;
                uint64_t available = pipe->size - pipe->read_offset;
                uint64_t to_copy = size < available ? size : available;
                memcpy(buffer, pipe->data + pipe->read_offset, to_copy);
                pipe->read_offset += to_copy;
                if (pipe->read_offset != 0) {
                    uint64_t remaining = pipe->size - pipe->read_offset;
                    if (remaining != 0) memmove(pipe->data, pipe->data + pipe->read_offset, remaining);
                    pipe->size = remaining;
                    pipe->read_offset = 0;
                }
                wake_pipe_waiters(entry.pipe_id, false, true);
                ++process->read_syscalls;
                process->read_bytes += to_copy;
                return to_copy;
            }
            return 0;
        }
    }
    return 0;
}

uint64_t UserspaceManager::write_file(uint64_t pid, uint32_t fd, const void* buffer, uint64_t size) {
    Process* process = find_process(pid);
    if (!process || !buffer || size == 0 || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return 0;
    for (auto& entry : process->file_descriptors) {
        if (entry.open && entry.fd == fd) {
            if (entry.kind == FileDescriptorKind::Vfs) {
                uint64_t bytes = hk::fs::vfs().write_handle(entry.vfs_handle, buffer, static_cast<size_t>(size));
                sync_descriptor_offsets(*process, entry.vfs_handle, hk::fs::vfs().handle_offset(entry.vfs_handle));
                ++process->write_syscalls;
                process->write_bytes += bytes;
                return bytes;
            }
            if (entry.kind == FileDescriptorKind::PipeWrite) {
                Pipe* pipe = find_pipe(entry.pipe_id);
                if (!pipe_has_live_reader(entry.pipe_id)) return 0;
                if (!pipe || pipe->size >= kPipeCapacity) return 0;
                uint64_t available = kPipeCapacity - pipe->size;
                uint64_t to_copy = size < available ? size : available;
                memcpy(pipe->data + pipe->size, buffer, to_copy);
                pipe->size += to_copy;
                wake_pipe_waiters(entry.pipe_id, true, false);
                ++process->write_syscalls;
                process->write_bytes += to_copy;
                return to_copy;
            }
            return 0;
        }
    }
    return 0;
}

bool UserspaceManager::seek_file(uint64_t pid, uint32_t fd, uint64_t offset) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    for (auto& entry : process->file_descriptors) {
        if (entry.open && entry.fd == fd) {
            if (entry.kind != FileDescriptorKind::Vfs) return false;
            if (!hk::fs::vfs().seek_handle(entry.vfs_handle, offset)) return false;
            sync_descriptor_offsets(*process, entry.vfs_handle, offset);
            return true;
        }
    }
    return false;
}

bool UserspaceManager::close_file(uint64_t pid, uint32_t fd) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty) return false;
    for (auto& entry : process->file_descriptors) {
        if (entry.open && entry.fd == fd) {
            uint32_t pipe_id = (entry.kind == FileDescriptorKind::PipeRead || entry.kind == FileDescriptorKind::PipeWrite) ? entry.pipe_id : 0;
            close_process_descriptor(*process, entry);
            if (pipe_id != 0) cleanup_pipe_if_unreferenced(pipe_id);
            return true;
        }
    }
    return false;
}

uint32_t UserspaceManager::duplicate_file(uint64_t pid, uint32_t source_fd) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return 0;
    const FileDescriptor* source = nullptr;
    for (const auto& fd : process->file_descriptors) {
        if (fd.open && fd.fd == source_fd) {
            source = &fd;
            break;
        }
    }
    if (!source) return 0;
    for (auto& fd : process->file_descriptors) {
        if (!fd.open) {
            uint32_t next_fd = process->next_fd++;
            if (next_fd < 3) next_fd = process->next_fd++;
            if (source->kind == FileDescriptorKind::Vfs && source->vfs_handle != 0 && !hk::fs::vfs().retain_handle(source->vfs_handle)) return 0;
            fd = *source;
            fd.fd = next_fd;
            return next_fd;
        }
    }
    return 0;
}

bool UserspaceManager::duplicate_file_to(uint64_t pid, uint32_t source_fd, uint32_t target_fd) {
    if (target_fd >= kMaxProcessFileDescriptors) return false;
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    const FileDescriptor* source = nullptr;
    for (const auto& fd : process->file_descriptors) {
        if (fd.open && fd.fd == source_fd) {
            source = &fd;
            break;
        }
    }
    if (!source) return false;
    if (source_fd == target_fd) return true;
    for (auto& fd : process->file_descriptors) {
        if (fd.open && fd.fd == target_fd) close_process_descriptor(*process, fd);
    }
    for (auto& fd : process->file_descriptors) {
        if (!fd.open) {
            if (source->kind == FileDescriptorKind::Vfs && source->vfs_handle != 0 && !hk::fs::vfs().retain_handle(source->vfs_handle)) return false;
            fd = *source;
            fd.fd = target_fd;
            if (target_fd >= process->next_fd) process->next_fd = target_fd + 1;
            return true;
        }
    }
    return false;
}

bool UserspaceManager::delete_file(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().delete_ram_file(resolved);
}

bool UserspaceManager::delete_directory(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().delete_ram_directory(resolved);
}

bool UserspaceManager::stat_path(uint64_t pid, const char* path, hybrid::VfsStatInfo& out) const {
    const Process* process = nullptr;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty && processes_[i].state != ProcessState::Exited) {
            process = &processes_[i];
            break;
        }
    }
    if (!process) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().stat(resolved, out);
}

bool UserspaceManager::copy_directory_entry(uint64_t pid, const char* path, uint32_t index, hybrid::VfsDirectoryEntryInfo& out) const {
    const Process* process = nullptr;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty && processes_[i].state != ProcessState::Exited) {
            process = &processes_[i];
            break;
        }
    }
    if (!process) return false;
    char resolved[64]{};
    if (!resolve_process_path(*process, path, resolved)) return false;
    return hk::fs::vfs().copy_directory_entry(resolved, index, out);
}

bool UserspaceManager::set_current_directory(uint64_t pid, const char* path) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    char next[64]{};
    if (!resolve_process_path(*process, path, next)) return false;
    const auto* node = hk::fs::vfs().find(next);
    if (!node || node->type != hk::fs::NodeType::Directory) return false;
    for (uint64_t i = 0; i < sizeof(process->current_directory); ++i) process->current_directory[i] = next[i];
    return true;
}

bool UserspaceManager::copy_current_directory(uint64_t pid, hybrid::PathInfo& out) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        for (uint64_t j = 0; j < sizeof(out.path); ++j) out.path[j] = processes_[i].current_directory[j];
        return out.path[0] == '/';
    }
    return false;
}

bool UserspaceManager::set_arguments(uint64_t pid, const char* const* args, uint32_t count) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (count > kMaxProcessArguments || (count > 0 && args == nullptr)) return false;
    clear_arguments(*process);
    for (uint32_t i = 0; i < count; ++i) {
        if (!copy_argument_bounded(process->arguments[i], args[i])) {
            clear_arguments(*process);
            return false;
        }
    }
    process->argument_count = count;
    return true;
}

uint64_t UserspaceManager::argument_count(uint64_t pid) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty && processes_[i].state != ProcessState::Exited) {
            return processes_[i].argument_count;
        }
    }
    return 0;
}

bool UserspaceManager::copy_argument(uint64_t pid, uint32_t index, hybrid::ArgumentInfo& out) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        if (index >= processes_[i].argument_count) return false;
        for (uint32_t j = 0; j < sizeof(out.value); ++j) out.value[j] = processes_[i].arguments[index][j];
        return out.value[0] != 0;
    }
    return false;
}

bool UserspaceManager::set_environment(uint64_t pid, const char* const* keys, const char* const* values, uint32_t count) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (count > kMaxEnvironmentEntries || (count > 0 && (!keys || !values))) return false;
    clear_environment(*process);
    for (uint32_t i = 0; i < count; ++i) {
        if (!valid_environment_key(keys[i]) ||
            !copy_text_bounded(process->environment_keys[i], keys[i]) ||
            !copy_text_bounded(process->environment_values[i], values[i])) {
            clear_environment(*process);
            return false;
        }
    }
    process->environment_count = count;
    return true;
}

bool UserspaceManager::set_environment_variable(uint64_t pid, const char* key, const char* value) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (!valid_environment_key(key) || !value || value[0] == 0) return false;
    for (uint32_t i = 0; i < process->environment_count; ++i) {
        if (!string_equal(process->environment_keys[i], key)) continue;
        return copy_text_bounded(process->environment_values[i], value);
    }
    if (process->environment_count >= kMaxEnvironmentEntries) return false;
    uint32_t index = process->environment_count;
    if (!copy_text_bounded(process->environment_keys[index], key) ||
        !copy_text_bounded(process->environment_values[index], value)) {
        return false;
    }
    process->environment_count++;
    return true;
}

bool UserspaceManager::unset_environment_variable(uint64_t pid, const char* key) {
    Process* process = find_process(pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    if (!valid_environment_key(key)) return false;
    for (uint32_t i = 0; i < process->environment_count; ++i) {
        if (!string_equal(process->environment_keys[i], key)) continue;
        for (uint32_t j = i; j + 1 < process->environment_count; ++j) {
            for (uint32_t k = 0; k < kMaxEnvironmentKeyLength; ++k) process->environment_keys[j][k] = process->environment_keys[j + 1][k];
            for (uint32_t k = 0; k < kMaxEnvironmentValueLength; ++k) process->environment_values[j][k] = process->environment_values[j + 1][k];
        }
        uint32_t last = process->environment_count - 1;
        for (uint32_t k = 0; k < kMaxEnvironmentKeyLength; ++k) process->environment_keys[last][k] = 0;
        for (uint32_t k = 0; k < kMaxEnvironmentValueLength; ++k) process->environment_values[last][k] = 0;
        process->environment_count--;
        return true;
    }
    return false;
}

bool UserspaceManager::inherit_environment(uint64_t child_pid, uint64_t parent_pid) {
    Process* child = find_process(child_pid);
    Process* parent = find_process(parent_pid);
    if (!child || !parent || child->state == ProcessState::Empty || parent->state == ProcessState::Empty || parent->state == ProcessState::Exited) return false;
    clear_environment(*child);
    for (uint32_t i = 0; i < parent->environment_count; ++i) {
        if (!copy_text_bounded(child->environment_keys[i], parent->environment_keys[i]) ||
            !copy_text_bounded(child->environment_values[i], parent->environment_values[i])) {
            clear_environment(*child);
            return false;
        }
    }
    child->environment_count = parent->environment_count;
    return true;
}

bool UserspaceManager::inherit_standard_fds(uint64_t child_pid, uint64_t parent_pid) {
    Process* child = find_process(child_pid);
    Process* parent = find_process(parent_pid);
    if (!child || !parent || child->state == ProcessState::Empty || child->state == ProcessState::Exited ||
        parent->state == ProcessState::Empty || parent->state == ProcessState::Exited) {
        return false;
    }

    for (uint32_t fd_number = hybrid::kStdinFd; fd_number <= hybrid::kStderrFd; ++fd_number) {
        const FileDescriptor* source = nullptr;
        for (const auto& fd : parent->file_descriptors) {
            if (fd.open && fd.fd == fd_number) {
                source = &fd;
                break;
            }
        }
        if (!source) continue;

        for (auto& fd : child->file_descriptors) {
            if (fd.open && fd.fd == fd_number) close_process_descriptor(*child, fd);
        }

        FileDescriptor inherited{};
        if (source->kind == FileDescriptorKind::Vfs) {
            if (source->path[0] == 0) return false;
            if (source->vfs_handle == 0 || !hk::fs::vfs().retain_handle(source->vfs_handle)) return false;
            inherited = make_vfs_descriptor(fd_number, source->vfs_handle, source->path, hk::fs::vfs().handle_offset(source->vfs_handle));
        } else if (source->kind == FileDescriptorKind::PipeRead || source->kind == FileDescriptorKind::PipeWrite) {
            if (!find_pipe(source->pipe_id)) return false;
            inherited = make_pipe_descriptor(fd_number, source->pipe_id, source->kind == FileDescriptorKind::PipeWrite);
        } else {
            continue;
        }

        bool installed = false;
        for (auto& fd : child->file_descriptors) {
            if (!fd.open) {
                fd = inherited;
                installed = true;
                break;
            }
        }
        if (!installed) {
            close_descriptor(inherited);
            return false;
        }
    }
    return true;
}

uint64_t UserspaceManager::environment_count(uint64_t pid) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty && processes_[i].state != ProcessState::Exited) {
            return processes_[i].environment_count;
        }
    }
    return 0;
}

bool UserspaceManager::copy_environment(uint64_t pid, uint32_t index, hybrid::EnvironmentInfo& out) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty || processes_[i].state == ProcessState::Exited) continue;
        if (index >= processes_[i].environment_count) return false;
        for (uint32_t j = 0; j < sizeof(out.key); ++j) out.key[j] = processes_[i].environment_keys[index][j];
        for (uint32_t j = 0; j < sizeof(out.value); ++j) out.value[j] = processes_[i].environment_values[index][j];
        return out.key[0] != 0 && out.value[0] != 0;
    }
    return false;
}

uint64_t UserspaceManager::open_file_count(uint64_t pid) const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid != pid || processes_[i].state == ProcessState::Empty) continue;
        for (const auto& fd : processes_[i].file_descriptors) if (fd.open) ++total;
        break;
    }
    return total;
}

uint64_t UserspaceManager::owned_page_count(uint64_t pid) const {
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty) return processes_[i].owned_page_count;
    }
    return 0;
}

bool UserspaceManager::copy_process_info(uint64_t index, hybrid::ProcessInfo& out) const {
    if (index >= count_) return false;
    const Process& process = processes_[index];
    if (process.state == ProcessState::Empty) return false;
    out.pid = process.pid;
    out.parent_pid = process.parent_pid;
    out.state = static_cast<uint32_t>(process.state);
    out.termination_reason = static_cast<uint32_t>(process.termination_reason);
    out.entry = process.entry;
    out.address_space_root = process.address_space_root;
    out.user_stack_top = process.user_stack_top;
    out.user_stack_pages = process.user_stack_pages;
    out.image_base = process.image_base;
    out.image_pages = process.image_pages;
    out.main_thread_id = process.main_thread_id;
    out.open_file_count = open_file_count(process.pid);
    out.owned_page_count = process.owned_page_count;
    out.exit_code = process.exit_code;
    out.process_group_id = process.process_group_id;
    out.syscall_count = process.syscall_count;
    out.last_syscall = process.last_syscall;
    out.read_syscalls = process.read_syscalls;
    out.write_syscalls = process.write_syscalls;
    out.read_bytes = process.read_bytes;
    out.write_bytes = process.write_bytes;
    out.run_ticks = process.run_ticks;
    out.switch_count = process.switch_count;
    out.preempt_count = process.preempt_count;
    copy_name(out.name, process.argument_count != 0 ? process.arguments[0] : process.name);
    return true;
}

bool UserspaceManager::copy_thread_info(uint64_t index, hybrid::UserThreadInfo& out) const {
    if (index >= thread_count_) return false;
    const UserThread& thread = threads_[index];
    if (thread.state == UserThreadState::Empty) return false;
    out.tid = thread.tid;
    out.pid = thread.pid;
    out.state = static_cast<uint32_t>(thread.state);
    out.block_reason = static_cast<uint32_t>(thread.block_reason);
    out.entry = thread.entry;
    out.user_stack_pointer = thread.user_stack_pointer;
    out.address_space_root = thread.address_space_root;
    out.wait_pipe_id = thread.wait_pipe_id;
    out.reserved = 0;
    out.wait_process_id = thread.block_reason == UserBlockReason::Sleep ? thread.wait_wake_tick : thread.wait_process_id;
    out.syscall_count = thread.syscall_count;
    out.last_syscall = thread.last_syscall;
    out.run_ticks = thread.run_ticks;
    out.switch_count = thread.switch_count;
    out.preempt_count = thread.preempt_count;
    return true;
}

bool UserspaceManager::copy_file_descriptor_info(uint64_t pid, uint64_t index, hybrid::FileDescriptorInfo& out) const {
    out = hybrid::FileDescriptorInfo{};
    if (index >= kMaxProcessFileDescriptors) return false;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.pid != pid || process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        const FileDescriptor& fd = process.file_descriptors[index];
        if (!fd.open) return false;
        out.pid = process.pid;
        out.fd = fd.fd;
        out.vfs_handle = fd.vfs_handle;
        out.pipe_id = fd.pipe_id;
        out.offset = fd.kind == FileDescriptorKind::Vfs ? hk::fs::vfs().handle_offset(fd.vfs_handle) : fd.offset;
        switch (fd.kind) {
        case FileDescriptorKind::Vfs:
            out.kind = hybrid::FileDescriptorInfoKind::Vfs;
            break;
        case FileDescriptorKind::PipeRead:
            out.kind = hybrid::FileDescriptorInfoKind::PipeRead;
            break;
        case FileDescriptorKind::PipeWrite:
            out.kind = hybrid::FileDescriptorInfoKind::PipeWrite;
            break;
        default:
            out.kind = hybrid::FileDescriptorInfoKind::Empty;
            break;
        }
        for (uint64_t j = 0; j < sizeof(out.path); ++j) out.path[j] = fd.path[j];
        return true;
    }
    return false;
}

uint64_t UserspaceManager::pipe_count() const {
    uint64_t total = 0;
    for (const auto& pipe : pipes_) {
        if (pipe.open) ++total;
    }
    return total;
}

bool UserspaceManager::copy_pipe_info(uint64_t index, hybrid::PipeInfo& out) const {
    out = hybrid::PipeInfo{};
    uint64_t seen = 0;
    const Pipe* selected = nullptr;
    for (const auto& pipe : pipes_) {
        if (!pipe.open) continue;
        if (seen++ == index) {
            selected = &pipe;
            break;
        }
    }
    if (!selected) return false;
    out.id = selected->id;
    out.open = selected->open ? 1u : 0u;
    out.size = selected->size;
    out.capacity = kPipeCapacity;
    out.read_offset = selected->read_offset;
    for (uint64_t i = 0; i < count_; ++i) {
        const Process& process = processes_[i];
        if (process.state == ProcessState::Empty || process.state == ProcessState::Exited) continue;
        for (const auto& fd : process.file_descriptors) {
            if (!fd.open || fd.pipe_id != selected->id) continue;
            if (fd.kind == FileDescriptorKind::PipeRead) ++out.reader_count;
            if (fd.kind == FileDescriptorKind::PipeWrite) ++out.writer_count;
        }
    }
    return true;
}

bool UserspaceManager::copy_launch_context(uint64_t tid, hybrid::LaunchContextInfo& out) {
    UserLaunchContext context{};
    if (!build_launch_context(tid, context)) return false;
    out.tid = context.tid;
    out.pid = context.pid;
    out.rip = context.rip;
    out.rsp = context.rsp;
    out.cr3 = context.cr3;
    out.cs = context.cs;
    out.ss = context.ss;
    out.reserved = 0;
    out.rflags = context.rflags;
    return true;
}

bool UserspaceManager::build_launch_context(uint64_t tid, UserLaunchContext& out) {
    UserThread* thread = find_thread(tid);
    if (!thread || (thread->state != UserThreadState::Runnable && thread->state != UserThreadState::Running)) return false;
    Process* process = find_process(thread->pid);
    if (!process || process->state != ProcessState::Runnable) return false;
    hk::mm::AddressSpace space{thread->address_space_root};
    if (hk::mm::translate(space, thread->entry) == 0) return false;
    if (hk::mm::translate(space, thread->user_stack_pointer) == 0) return false;
    out = UserLaunchContext{
        thread->tid,
        thread->pid,
        thread->entry,
        thread->user_stack_pointer,
        thread->address_space_root,
        hk::cpu::kUserCodeSelector,
        hk::cpu::kUserDataSelector,
        thread->rflags != 0 ? thread->rflags : 0x202,
    };
    return true;
}

bool UserspaceManager::select_next_runnable_thread(UserLaunchContext& out) {
    if (thread_count_ == 0) return false;
    for (uint64_t step = 1; step <= thread_count_; ++step) {
        uint64_t index = (last_user_pick_index_ + step) % thread_count_;
        UserThread& thread = threads_[index];
        if (thread.state != UserThreadState::Runnable) continue;
        Process* process = find_process(thread.pid);
        if (!process || process->state != ProcessState::Runnable) continue;
        if (!build_launch_context(thread.tid, out)) return false;
        last_user_pick_index_ = index;
        last_selected_tid_ = thread.tid;
        return true;
    }
    return false;
}

bool UserspaceManager::build_process_launch_context(uint64_t pid, UserLaunchContext& out) {
    Process* process = find_process(pid);
    if (!process || process->state != ProcessState::Runnable || process->main_thread_id == 0) return false;
    return build_launch_context(process->main_thread_id, out);
}

uint64_t UserspaceManager::runnable_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < count_; ++i) if (processes_[i].state == ProcessState::Runnable) ++total;
    return total;
}

uint64_t UserspaceManager::live_process_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].state != ProcessState::Empty && processes_[i].state != ProcessState::Exited) ++total;
    }
    return total;
}

uint64_t UserspaceManager::exited_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < count_; ++i) if (processes_[i].state == ProcessState::Exited) ++total;
    return total;
}

uint64_t UserspaceManager::runnable_thread_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) if (threads_[i].state == UserThreadState::Runnable) ++total;
    return total;
}

uint64_t UserspaceManager::running_thread_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) if (threads_[i].state == UserThreadState::Running) ++total;
    return total;
}

uint64_t UserspaceManager::schedulable_thread_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) {
        const UserThread& thread = threads_[i];
        if (thread.state != UserThreadState::Runnable && thread.state != UserThreadState::Running) continue;
        for (uint64_t j = 0; j < count_; ++j) {
            const Process& process = processes_[j];
            if (process.pid == thread.pid && process.state == ProcessState::Runnable) {
                ++total;
                break;
            }
        }
    }
    return total;
}

bool UserspaceManager::update_user_preemption_gate() {
    bool should_enable = schedulable_thread_count() > 1;
    if (should_enable) {
        if (!hk::timer::lapic_timer_active() && !hk::timer::start_lapic_system_tick(0x400000)) return false;
        if (!hk::timer::user_preemption_enabled()) {
            hk::timer::set_user_preemption_enabled(true);
            ++diagnostics_.preemption_gate_enables;
            hk::log_hex(hk::LogLevel::Info, "Userspace preemption gate enable count", diagnostics_.preemption_gate_enables);
            hk::log(hk::LogLevel::Info, "Userspace auto preemption enabled");
            hk::log(hk::LogLevel::Info, "Userspace scheduler preemption gate enabled");
        }
        return true;
    }
    if (hk::timer::user_preemption_enabled()) {
        hk::timer::set_user_preemption_enabled(false);
        ++diagnostics_.preemption_gate_disables;
        hk::log_hex(hk::LogLevel::Info, "Userspace preemption gate disable count", diagnostics_.preemption_gate_disables);
        hk::log(hk::LogLevel::Info, "Userspace scheduler preemption gate disabled");
    }
    return true;
}

uint64_t UserspaceManager::exited_thread_count() const {
    uint64_t total = 0;
    for (uint64_t i = 0; i < thread_count_; ++i) if (threads_[i].state == UserThreadState::Exited) ++total;
    return total;
}

bool UserspaceManager::copy_user_scheduler_info(hybrid::UserSchedulerInfo& out) const {
    out.current_tid = current_tid();
    out.current_pid = current_pid();
    out.runnable_threads = runnable_thread_count();
    out.running_threads = running_thread_count();
    out.exited_threads = exited_thread_count();
    out.last_selected_tid = last_selected_tid_;
    out.schedulable_threads = schedulable_thread_count();
    out.timeslice_quantum = kUserTimesliceQuantum;
    out.current_slice_ticks = current_slice_ticks_;
    out.expired_slices = expired_slices_;
    return true;
}

bool UserspaceManager::copy_current_user_context(hybrid::CurrentUserContextInfo& out) const {
    out = hybrid::CurrentUserContextInfo{};
    uint64_t pid = current_pid();
    uint64_t tid = current_tid();
    if (pid == 0 || tid == 0) return false;
    const Process* process = nullptr;
    const UserThread* thread = nullptr;
    for (uint64_t i = 0; i < count_; ++i) {
        if (processes_[i].pid == pid && processes_[i].state != ProcessState::Empty) {
            process = &processes_[i];
            break;
        }
    }
    for (uint64_t i = 0; i < thread_count_; ++i) {
        if (threads_[i].tid == tid && threads_[i].state != UserThreadState::Empty) {
            thread = &threads_[i];
            break;
        }
    }
    if (!process || !thread || thread->pid != process->pid) return false;
    out.pid = process->pid;
    out.tid = thread->tid;
    out.process_state = static_cast<uint32_t>(process->state);
    out.thread_state = static_cast<uint32_t>(thread->state);
    out.entry = thread->entry;
    out.user_stack_pointer = thread->user_stack_pointer;
    out.address_space_root = thread->address_space_root;
    return true;
}

bool UserspaceManager::record_current_syscall(uint64_t number) {
    UserThread* thread = current_thread();
    if (!thread || thread->state == UserThreadState::Empty || thread->state == UserThreadState::Exited) return false;
    Process* process = find_process(thread->pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    ++thread->syscall_count;
    thread->last_syscall = number;
    ++process->syscall_count;
    process->last_syscall = number;
    return true;
}

bool UserspaceManager::note_current_user_tick() {
    UserThread* thread = current_thread();
    if (!thread || thread->state == UserThreadState::Empty || thread->state == UserThreadState::Exited) return false;
    Process* process = find_process(thread->pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    ++thread->run_ticks;
    ++process->run_ticks;
    ++current_slice_ticks_;
    return true;
}

bool UserspaceManager::user_timeslice_expired() {
    if (schedulable_thread_count() <= 1) {
        current_slice_ticks_ = 0;
        return false;
    }
    if (current_slice_ticks_ < kUserTimesliceQuantum) return false;
    current_slice_ticks_ = 0;
    ++expired_slices_;
    return true;
}

bool UserspaceManager::note_user_preempt_switch(uint64_t from_tid, uint64_t to_tid) {
    if (from_tid == 0 || to_tid == 0 || from_tid == to_tid) return false;
    UserThread* from = find_thread(from_tid);
    if (!from || from->state == UserThreadState::Empty || from->state == UserThreadState::Exited) return false;
    Process* process = find_process(from->pid);
    if (!process || process->state == ProcessState::Empty || process->state == ProcessState::Exited) return false;
    ++from->preempt_count;
    ++process->preempt_count;
    ++diagnostics_.preempt_switches;
    hk::log_hex(hk::LogLevel::Info, "Userspace preempt switch count", diagnostics_.preempt_switches);
    return true;
}

bool validate_process_mappings(const Process& process) {
    if (process.address_space_root == 0 || process.entry == 0 || process.user_stack_top == 0) return false;
    auto* main_thread = userspace_manager().find_thread(process.main_thread_id);
    if (!main_thread || main_thread->pid != process.pid || main_thread->entry != process.entry) return false;
    if (main_thread->address_space_root != process.address_space_root || main_thread->user_stack_pointer >= process.user_stack_top) return false;
    hk::mm::AddressSpace space{process.address_space_root};
    if (hk::mm::translate(space, process.entry) == 0) return false;
    if (hk::mm::translate(space, process.user_stack_top - 8) == 0) return false;
    uint64_t entry_flags = hk::mm::page_flags(space, process.entry);
    if ((entry_flags & hk::mm::PageUser) == 0 || (entry_flags & hk::mm::PageNoExecute) != 0) return false;
    uint64_t stack_flags = hk::mm::page_flags(space, process.user_stack_top - 8);
    if ((stack_flags & hk::mm::PageUser) == 0 || (stack_flags & hk::mm::PageWrite) == 0) return false;
    return true;
}

bool process_lifecycle_self_test() {
    auto& manager = userspace_manager();
    auto* process = manager.create_process_stub("lifecycle-selftest", 0x401000, 0x2000);
    if (!process || process->state != ProcessState::Created) return false;
    uint64_t pid = process->pid;
    if (manager.find_process(pid) != process) return false;
    static const char* test_args[] = {"selftest", "--flag"};
    if (!manager.set_arguments(pid, test_args, 2) || manager.argument_count(pid) != 2) return false;
    hybrid::ArgumentInfo arg{};
    if (!manager.copy_argument(pid, 1, arg) || arg.value[0] != '-' || arg.value[1] != '-' || arg.value[2] != 'f') return false;
    static const char* test_env_keys[] = {"MODE", "ROOT"};
    static const char* test_env_values[] = {"selftest", "/"};
    if (!manager.set_environment(pid, test_env_keys, test_env_values, 2) || manager.environment_count(pid) != 2) return false;
    hybrid::EnvironmentInfo env{};
    if (!manager.copy_environment(pid, 0, env) || env.key[0] != 'M' || env.key[1] != 'O' ||
        env.value[0] != 's' || env.value[1] != 'e') return false;
    if (!manager.mark_runnable(pid) || process->state != ProcessState::Runnable) return false;
    if (manager.runnable_count() == 0) return false;
    if (!manager.exit_process(pid, 0x2a) || process->state != ProcessState::Exited || process->exit_code != 0x2a) return false;
    if (manager.exited_count() == 0) return false;
    if (!manager.reap_exited(pid)) return false;
    if (manager.find_process(pid) != nullptr) return false;
    auto* reused = manager.create_process_stub("lifecycle-reuse-selftest", 0x402000, 0x3000);
    if (reused != process || reused->state != ProcessState::Created || reused->pid == pid) return false;
    if (!manager.exit_process(reused->pid, 0) || !manager.reap_exited(reused->pid)) return false;
    auto* owner = manager.create_process("page-owner-selftest", 0x403000);
    if (!owner || owner->owned_page_count != kDefaultUserStackPages) return false;
    uint64_t owner_pid = owner->pid;
    uint64_t owned_pages = owner->owned_page_count;
    uint64_t free_before_reap = hk::mm::pmm().free_pages();
    if (!manager.exit_process(owner_pid, 0) || !manager.reap_exited(owner_pid)) return false;
    if (hk::mm::pmm().free_pages() < free_before_reap + owned_pages) return false;
    auto* threaded = manager.create_process("thread-reap-selftest", 0x405000);
    if (!threaded || threaded->main_thread_id == 0) return false;
    uint64_t first_thread_slots = manager.user_thread_count();
    uint64_t first_tid = threaded->main_thread_id;
    if (!manager.find_thread(first_tid)) return false;
    if (!manager.exit_process(threaded->pid, 0) || !manager.reap_exited(threaded->pid)) return false;
    if (manager.find_thread(first_tid) != nullptr) return false;
    auto* reused_thread_slot = manager.create_process("thread-reuse-selftest", 0x406000);
    if (!reused_thread_slot || reused_thread_slot->main_thread_id == 0) return false;
    if (manager.user_thread_count() != first_thread_slots) return false;
    if (reused_thread_slot->main_thread_id == first_tid) return false;
    if (!manager.exit_process(reused_thread_slot->pid, 0) || !manager.reap_exited(reused_thread_slot->pid)) return false;
    hk::log(hk::LogLevel::Info, "user thread reap self-test");
    return true;
}

bool launch_context_self_test() {
    auto& manager = userspace_manager();
    for (uint64_t pid = 1; pid <= manager.process_count(); ++pid) {
        Process* process = manager.find_process(pid);
        if (!process || process->state != ProcessState::Runnable || process->main_thread_id == 0) continue;
        UserLaunchContext context{};
        if (!manager.build_launch_context(process->main_thread_id, context)) return false;
        if (context.tid != process->main_thread_id || context.pid != process->pid) return false;
        if (context.rip != process->entry || context.rsp >= process->user_stack_top) return false;
        if (context.cr3 != process->address_space_root) return false;
        if (context.cs != hk::cpu::kUserCodeSelector || context.ss != hk::cpu::kUserDataSelector) return false;
        if ((context.rflags & 0x200) == 0) return false;
        if (process->pid == manager.active_pid()) {
            if (manager.active_tid() != process->main_thread_id) return false;
            if (!manager.save_active_thread_frame(context.rip, context.rsp)) return false;
            UserLaunchContext saved_context{};
            if (!manager.build_launch_context(process->main_thread_id, saved_context)) return false;
            if (saved_context.rip != context.rip || saved_context.rsp != context.rsp || saved_context.cr3 != context.cr3) return false;
            hybrid::CurrentUserContextInfo current{};
            if (!manager.copy_current_user_context(current) ||
                current.pid != process->pid ||
                current.tid != process->main_thread_id ||
                current.address_space_root != process->address_space_root) {
                return false;
            }
            hk::log(hk::LogLevel::Info, "thread-derived process context self-test");
            hk::log(hk::LogLevel::Info, "user thread frame save self-test");
        }
        return true;
    }
    return false;
}

bool file_descriptor_self_test() {
    auto& manager = userspace_manager();
    uint64_t pid = manager.active_pid();
    if (pid == 0) return false;
    uint64_t baseline_fds = manager.open_file_count(pid);
    if (baseline_fds < 3) return false;
    for (uint32_t fd_number = hybrid::kStdinFd; fd_number <= hybrid::kStderrFd; ++fd_number) {
        hybrid::FileDescriptorInfo stdio{};
        bool saw_stdio = false;
        for (uint64_t i = 0; i < kMaxProcessFileDescriptors; ++i) {
            if (!manager.copy_file_descriptor_info(pid, i, stdio)) continue;
            if (stdio.fd == fd_number &&
                stdio.kind == hybrid::FileDescriptorInfoKind::Vfs &&
                stdio.path[0] == '/' && stdio.path[1] == 'd' && stdio.path[2] == 'e' &&
                stdio.path[3] == 'v' && stdio.path[4] == '/' && stdio.path[5] == 't') {
                saw_stdio = true;
                break;
            }
        }
        if (!saw_stdio) return false;
    }
    hk::log(hk::LogLevel::Info, "default tty stdio self-test");
    uint32_t fd = manager.open_file(pid, "/user/init.elf");
    if (fd < 3 || manager.open_file_count(pid) != baseline_fds + 1) return false;
    unsigned char magic[4]{};
    if (manager.read_file(pid, fd, magic, sizeof(magic)) != sizeof(magic)) return false;
    if (magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' || magic[3] != 'F') return false;
    if (!manager.seek_file(pid, fd, 0)) return false;
    unsigned char rewind_magic[4]{};
    if (manager.read_file(pid, fd, rewind_magic, sizeof(rewind_magic)) != sizeof(rewind_magic)) return false;
    if (rewind_magic[0] != 0x7f || rewind_magic[1] != 'E' || rewind_magic[2] != 'L' || rewind_magic[3] != 'F') return false;
    if (!manager.close_file(pid, fd) || manager.open_file_count(pid) != baseline_fds) return false;
    if (!manager.set_current_directory(pid, "/user")) return false;
    uint32_t relative_fd = manager.open_file(pid, "init.elf");
    if (relative_fd < 3 || manager.open_file_count(pid) != baseline_fds + 1) return false;
    unsigned char relative_magic[4]{};
    if (manager.read_file(pid, relative_fd, relative_magic, sizeof(relative_magic)) != sizeof(relative_magic)) return false;
    if (relative_magic[0] != 0x7f || relative_magic[1] != 'E' || relative_magic[2] != 'L' || relative_magic[3] != 'F') return false;
    if (!manager.close_file(pid, relative_fd) || manager.open_file_count(pid) != baseline_fds) return false;
    uint32_t dotted_fd = manager.open_file(pid, "./init.elf");
    if (dotted_fd < 3 || manager.open_file_count(pid) != baseline_fds + 1) return false;
    unsigned char dotted_magic[4]{};
    if (manager.read_file(pid, dotted_fd, dotted_magic, sizeof(dotted_magic)) != sizeof(dotted_magic)) return false;
    if (dotted_magic[0] != 0x7f || dotted_magic[1] != 'E' || dotted_magic[2] != 'L' || dotted_magic[3] != 'F') return false;
    if (!manager.close_file(pid, dotted_fd) || manager.open_file_count(pid) != baseline_fds) return false;
    uint32_t normalized_fd = manager.open_file(pid, "/user/../user/./init.elf");
    if (normalized_fd < 3 || manager.open_file_count(pid) != baseline_fds + 1) return false;
    if (!manager.close_file(pid, normalized_fd) || manager.open_file_count(pid) != baseline_fds) return false;
    uint32_t dup_source = manager.open_file(pid, "/user/init.elf");
    if (dup_source < 3) return false;
    uint32_t dup_fd = manager.duplicate_file(pid, dup_source);
    if (dup_fd < 3 || dup_fd == dup_source || manager.open_file_count(pid) != baseline_fds + 2) return false;
    hybrid::FileDescriptorInfo dup_info{};
    bool saw_dup = false;
    for (uint64_t i = 0; i < kMaxProcessFileDescriptors; ++i) {
        if (!manager.copy_file_descriptor_info(pid, i, dup_info)) continue;
        if (dup_info.fd == dup_fd &&
            dup_info.kind == hybrid::FileDescriptorInfoKind::Vfs &&
            dup_info.vfs_handle != 0 &&
            dup_info.path[0] == '/' &&
            dup_info.path[1] == 'u') {
            saw_dup = true;
        }
    }
    if (!saw_dup) return false;
    if (!manager.duplicate_file_to(pid, dup_source, hybrid::kStderrFd)) return false;
    hybrid::FileDescriptorInfo dup2_info{};
    bool saw_dup2 = false;
    for (uint64_t i = 0; i < kMaxProcessFileDescriptors; ++i) {
        if (!manager.copy_file_descriptor_info(pid, i, dup2_info)) continue;
        if (dup2_info.fd == hybrid::kStderrFd && dup2_info.kind == hybrid::FileDescriptorInfoKind::Vfs) saw_dup2 = true;
    }
    if (!saw_dup2 || manager.open_file_count(pid) != baseline_fds + 2) return false;
    if (!manager.close_file(pid, dup_fd)) return false;
    if (!manager.close_file(pid, hybrid::kStderrFd)) return false;
    if (!manager.close_file(pid, dup_source) || manager.open_file_count(pid) != baseline_fds - 1) return false;
    if (!manager.redirect_file(pid, hybrid::kStderrFd, "/dev/tty") || manager.open_file_count(pid) != baseline_fds) return false;
    hk::log(hk::LogLevel::Info, "fd dup info self-test");
    if (!manager.set_current_directory(pid, "..")) return false;
    hybrid::PathInfo normalized_cwd{};
    if (!manager.copy_current_directory(pid, normalized_cwd) || normalized_cwd.path[0] != '/' || normalized_cwd.path[1] != 0) return false;
    hk::log(hk::LogLevel::Info, "path normalization self-test");
    if (!manager.set_current_directory(pid, "/")) return false;
    if (!manager.create_file(pid, "/tmp/fdredir.txt")) return false;
    if (!manager.redirect_file(pid, hybrid::kStdoutFd, "/tmp/fdredir.txt")) return false;
    static const char redirected[] = "redirected\n";
    if (manager.write_file(pid, hybrid::kStdoutFd, redirected, sizeof(redirected) - 1) != sizeof(redirected) - 1) return false;
    if (!manager.close_file(pid, hybrid::kStdoutFd)) return false;
    uint32_t redirected_fd = manager.open_file(pid, "/tmp/fdredir.txt");
    if (redirected_fd < 3) return false;
    char redirected_read[sizeof(redirected)]{};
    if (manager.read_file(pid, redirected_fd, redirected_read, sizeof(redirected) - 1) != sizeof(redirected) - 1) return false;
    if (redirected_read[0] != 'r' || redirected_read[1] != 'e' || redirected_read[9] != 'd') return false;
    if (!manager.close_file(pid, redirected_fd)) return false;
    if (!manager.delete_file(pid, "/tmp/fdredir.txt")) return false;
    if (!manager.redirect_file(pid, hybrid::kStdoutFd, "/dev/tty") || manager.open_file_count(pid) != baseline_fds) return false;
    if (!manager.create_file(pid, "/tmp/fdinherit.txt")) return false;
    if (!manager.redirect_file(pid, hybrid::kStdoutFd, "/tmp/fdinherit.txt")) return false;
    static const char parent_write[] = "parent\n";
    if (manager.write_file(pid, hybrid::kStdoutFd, parent_write, sizeof(parent_write) - 1) != sizeof(parent_write) - 1) return false;
    auto* child = manager.create_process_stub("fd-inherit-selftest", 0x404000, 0x5000);
    if (!child) return false;
    uint64_t child_pid = child->pid;
    if (!manager.set_parent(child_pid, pid)) return false;
    if (!manager.inherit_standard_fds(child_pid, pid)) return false;
    static const char child_write[] = "child\n";
    if (manager.write_file(child_pid, hybrid::kStdoutFd, child_write, sizeof(child_write) - 1) != sizeof(child_write) - 1) return false;
    if (!manager.exit_process(child_pid, 0) || !manager.reap_exited(child_pid)) return false;
    if (!manager.close_file(pid, hybrid::kStdoutFd)) return false;
    uint32_t inherited_fd = manager.open_file(pid, "/tmp/fdinherit.txt");
    if (inherited_fd < 3) return false;
    char inherited_read[sizeof(parent_write) + sizeof(child_write)]{};
    if (manager.read_file(pid, inherited_fd, inherited_read, sizeof(parent_write) + sizeof(child_write) - 2) != sizeof(parent_write) + sizeof(child_write) - 2) return false;
    if (inherited_read[0] != 'p' || inherited_read[6] != '\n' || inherited_read[7] != 'c' || inherited_read[12] != '\n') return false;
    if (!manager.close_file(pid, inherited_fd)) return false;
    if (!manager.delete_file(pid, "/tmp/fdinherit.txt")) return false;
    if (!manager.redirect_file(pid, hybrid::kStdoutFd, "/dev/tty") || manager.open_file_count(pid) != baseline_fds) return false;
    hk::log(hk::LogLevel::Info, "fd inheritance self-test");
    return true;
}
}
