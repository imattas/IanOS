#include "hybrid/syscall.hpp"

namespace {

uint64_t last_spawn_pid = 0;
constexpr uint64_t kLineCapacity = 64;
constexpr uint64_t kHistoryCapacity = 8;
const char* shell_prompt_text = "ianos> ";
char shell_history[kHistoryCapacity][kLineCapacity]{};
uint64_t shell_history_count = 0;
uint64_t shell_history_next = 0;
bool shell_trace_commands = false;
bool shell_tag_output = false;
uint64_t shell_last_status = 0;
uint64_t shell_process_group_id = 0;
constexpr uint64_t kJobCapacity = 8;
constexpr uint64_t kMaxJobProcesses = 4;

struct ShellJob {
    uint64_t job_id;
    uint64_t pid;
    uint64_t process_group_id;
    uint64_t process_count;
    uint64_t pids[kMaxJobProcesses];
    uint64_t last_status;
    bool active;
    bool waited;
    char command[64];
};

ShellJob shell_jobs[kJobCapacity]{};
uint64_t next_job_id = 1;

void run_shell_line(char* line);

hybrid::SyscallResult syscall(hybrid::SyscallNumber number, uint64_t arg0 = 0, uint64_t arg1 = 0, uint64_t arg2 = 0, uint64_t arg3 = 0) {
    uint64_t value = 0;
    uint64_t error = 0;
    register uint64_t rax asm("rax") = static_cast<uint64_t>(number);
    register uint64_t rdi asm("rdi") = arg0;
    register uint64_t rsi asm("rsi") = arg1;
    register uint64_t rdx asm("rdx") = arg2;
    register uint64_t r10 asm("r10") = arg3;
    asm volatile(
        "int $0x80"
        : "+a"(rax), "+d"(rdx)
        : "D"(rdi), "S"(rsi), "r"(r10)
        : "rcx", "r8", "r9", "r11", "memory");
    value = rax;
    error = rdx;
    return {value, error};
}

void log(const char* message) {
    uint64_t length = 0;
    while (message[length] != 0) ++length;
    syscall(hybrid::SyscallNumber::DebugLog, reinterpret_cast<uint64_t>(message), length + 1);
}

void write_terminal(const char* message) {
    uint64_t length = 0;
    while (message[length] != 0) ++length;
    syscall(hybrid::SyscallNumber::Write, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(message), length);
}

void shell_emit(const char* message) {
    write_terminal(message);
    write_terminal("\n");
}

uint64_t strlen(const char* text) {
    uint64_t length = 0;
    while (text[length] != 0) ++length;
    return length;
}

bool streq(const char* left, const char* right) {
    uint64_t index = 0;
    while (left[index] != 0 && right[index] != 0) {
        if (left[index] != right[index]) return false;
        ++index;
    }
    return left[index] == 0 && right[index] == 0;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
    for (++i; i < capacity; ++i) out[i] = 0;
}

uint64_t parse_number(const char* text, bool& ok) {
    ok = false;
    if (!text || text[0] == 0) return 0;
    uint64_t value = 0;
    uint64_t index = 0;
    uint64_t base = 10;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        index = 2;
    }
    for (; text[index] != 0; ++index) {
        char c = text[index];
        uint64_t digit = 0;
        if (c >= '0' && c <= '9') digit = static_cast<uint64_t>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = 10 + static_cast<uint64_t>(c - 'a');
        else if (base == 16 && c >= 'A' && c <= 'F') digit = 10 + static_cast<uint64_t>(c - 'A');
        else return 0;
        if (digit >= base) return 0;
        value = value * base + digit;
        ok = true;
    }
    return value;
}

char hex_digit(uint64_t value) {
    value &= 0xf;
    return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + (value - 10));
}

void append_char(char* buffer, uint64_t capacity, uint64_t& cursor, char value) {
    if (cursor + 1 >= capacity) return;
    buffer[cursor++] = value;
    buffer[cursor] = 0;
}

void append_text(char* buffer, uint64_t capacity, uint64_t& cursor, const char* text) {
    for (uint64_t i = 0; text[i] != 0; ++i) append_char(buffer, capacity, cursor, text[i]);
}

void append_hex(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    append_text(buffer, capacity, cursor, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) append_char(buffer, capacity, cursor, hex_digit(value >> shift));
}

const char* process_state_name(uint32_t state) {
    switch (state) {
    case 0: return "empty";
    case 1: return "created";
    case 2: return "runnable";
    case 3: return "stopped";
    case 4: return "exited";
    default: return "unknown";
    }
}

const char* process_reason_name(uint32_t reason) {
    switch (reason) {
    case 0: return "none";
    case 1: return "exit";
    case 9: return "sigkill";
    case 15: return "sigterm";
    default: return "unknown";
    }
}

ShellJob* find_job(uint64_t pid) {
    if (pid == 0) return nullptr;
    for (uint64_t i = 0; i < kJobCapacity; ++i) {
        if (!shell_jobs[i].active) continue;
        if (shell_jobs[i].pid == pid) return &shell_jobs[i];
        for (uint64_t j = 0; j < shell_jobs[i].process_count && j < kMaxJobProcesses; ++j) {
            if (shell_jobs[i].pids[j] == pid) return &shell_jobs[i];
        }
    }
    return nullptr;
}

ShellJob* find_job_id(uint64_t job_id) {
    if (job_id == 0) return nullptr;
    for (uint64_t i = 0; i < kJobCapacity; ++i) {
        if (shell_jobs[i].active && shell_jobs[i].job_id == job_id) return &shell_jobs[i];
    }
    return nullptr;
}

ShellJob* current_job() {
    ShellJob* newest = nullptr;
    for (uint64_t i = 0; i < kJobCapacity; ++i) {
        if (!shell_jobs[i].active) continue;
        if (!newest || shell_jobs[i].job_id > newest->job_id) newest = &shell_jobs[i];
    }
    return newest;
}

void remember_job(uint64_t pid, const char* command) {
    if (pid == 0) return;
    ShellJob* slot = find_job(pid);
    if (!slot) {
        for (uint64_t i = 0; i < kJobCapacity; ++i) {
            if (!shell_jobs[i].active) {
                slot = &shell_jobs[i];
                break;
            }
        }
    }
    if (!slot) slot = &shell_jobs[0];
    uint64_t job_id = slot->active && slot->job_id != 0 ? slot->job_id : next_job_id++;
    if (job_id == 0) job_id = next_job_id++;
    slot->job_id = job_id;
    slot->pid = pid;
    slot->process_group_id = pid;
    slot->process_count = 1;
    for (uint64_t i = 0; i < kMaxJobProcesses; ++i) slot->pids[i] = i == 0 ? pid : 0;
    slot->last_status = 0;
    slot->active = true;
    slot->waited = false;
    copy_text(slot->command, sizeof(slot->command), command);
}

void remember_pipeline_job(uint64_t process_group_id, const uint64_t* pids, uint64_t process_count, const char* command) {
    if (process_group_id == 0 || !pids || process_count == 0) return;
    ShellJob* slot = find_job(process_group_id);
    if (!slot) {
        for (uint64_t i = 0; i < kJobCapacity; ++i) {
            if (!shell_jobs[i].active) {
                slot = &shell_jobs[i];
                break;
            }
        }
    }
    if (!slot) slot = &shell_jobs[0];
    uint64_t job_id = slot->active && slot->job_id != 0 ? slot->job_id : next_job_id++;
    if (job_id == 0) job_id = next_job_id++;
    slot->job_id = job_id;
    slot->pid = process_group_id;
    slot->process_group_id = process_group_id;
    slot->process_count = process_count > kMaxJobProcesses ? kMaxJobProcesses : process_count;
    for (uint64_t i = 0; i < kMaxJobProcesses; ++i) slot->pids[i] = i < slot->process_count ? pids[i] : 0;
    slot->last_status = 0;
    slot->active = true;
    slot->waited = false;
    copy_text(slot->command, sizeof(slot->command), command);
}

void forget_job(uint64_t pid) {
    ShellJob* job = find_job(pid);
    if (!job) return;
    job->job_id = 0;
    job->pid = 0;
    job->process_group_id = 0;
    job->process_count = 0;
    for (uint64_t i = 0; i < kMaxJobProcesses; ++i) job->pids[i] = 0;
    job->last_status = 0;
    job->active = false;
    job->waited = false;
    copy_text(job->command, sizeof(job->command), "");
}

bool process_info_by_pid(uint64_t pid, hybrid::ProcessInfo& out) {
    auto count = syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ProcessInfo info;
        auto result = syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&info));
        if (result.error == hybrid::kSyscallErrorNone && info.pid == pid) {
            auto* dst = reinterpret_cast<unsigned char*>(&out);
            auto* src = reinterpret_cast<unsigned char*>(&info);
            for (uint64_t j = 0; j < sizeof(out); ++j) dst[j] = src[j];
            return true;
        }
    }
    return false;
}

uint64_t command_pid_or_last(const char* argument);
bool poll_job_control_for_process_group(uint64_t process_group_id);

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) append_char(buffer, capacity, cursor, digits[--count]);
}

void shell_line(const char* prefix, const char* text) {
    char line[256];
    uint64_t cursor = 0;
    if (shell_tag_output) append_text(line, sizeof(line), cursor, "[shell] ");
    append_text(line, sizeof(line), cursor, prefix);
    append_text(line, sizeof(line), cursor, text);
    shell_emit(line);
}

void shell_decimal(const char* prefix, uint64_t value) {
    char line[128];
    uint64_t cursor = 0;
    if (shell_tag_output) append_text(line, sizeof(line), cursor, "[shell] ");
    append_text(line, sizeof(line), cursor, prefix);
    append_decimal(line, sizeof(line), cursor, value);
    shell_emit(line);
}

void shell_hex(const char* label, uint64_t value) {
    char line[128];
    uint64_t cursor = 0;
    if (shell_tag_output) append_text(line, sizeof(line), cursor, "[shell] ");
    append_text(line, sizeof(line), cursor, label);
    append_hex(line, sizeof(line), cursor, value);
    shell_emit(line);
}

void shell_path(const char* label, const char* value) {
    char line[128];
    uint64_t cursor = 0;
    if (shell_tag_output) append_text(line, sizeof(line), cursor, "[shell] ");
    append_text(line, sizeof(line), cursor, label);
    append_text(line, sizeof(line), cursor, value);
    shell_emit(line);
}

constexpr uint64_t kFastfetchRows = 16;
constexpr uint64_t kFastfetchDetailCapacity = 96;
void add_fetch_text(char (&details)[kFastfetchRows][kFastfetchDetailCapacity], uint64_t& count, const char* key, const char* value) {
    if (count >= kFastfetchRows) return;
    uint64_t cursor = 0;
    append_text(details[count], kFastfetchDetailCapacity, cursor, key);
    append_text(details[count], kFastfetchDetailCapacity, cursor, ": ");
    append_text(details[count], kFastfetchDetailCapacity, cursor, value ? value : "");
    ++count;
}

void add_fetch_hex(char (&details)[kFastfetchRows][kFastfetchDetailCapacity], uint64_t& count, const char* key, uint64_t value) {
    if (count >= kFastfetchRows) return;
    uint64_t cursor = 0;
    append_text(details[count], kFastfetchDetailCapacity, cursor, key);
    append_text(details[count], kFastfetchDetailCapacity, cursor, ": ");
    append_hex(details[count], kFastfetchDetailCapacity, cursor, value);
    ++count;
}

void add_fetch_decimal(char (&details)[kFastfetchRows][kFastfetchDetailCapacity], uint64_t& count, const char* key, uint64_t value) {
    if (count >= kFastfetchRows) return;
    uint64_t cursor = 0;
    append_text(details[count], kFastfetchDetailCapacity, cursor, key);
    append_text(details[count], kFastfetchDetailCapacity, cursor, ": ");
    append_decimal(details[count], kFastfetchDetailCapacity, cursor, value);
    ++count;
}

void write_fastfetch_banner() {
    hybrid::SystemInfo system{};
    hybrid::MemoryStatsInfo memory{};
    hybrid::FramebufferInfo fb{};
    auto system_result = syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&system));
    auto memory_result = syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&memory));
    auto cpu_count = syscall(hybrid::SyscallNumber::GetCpuCount);
    auto device_count = syscall(hybrid::SyscallNumber::GetDeviceCount);
    auto storage_count = syscall(hybrid::SyscallNumber::GetStorageDeviceCount);
    auto network_count = syscall(hybrid::SyscallNumber::GetNetworkDeviceCount);
    auto display_count = syscall(hybrid::SyscallNumber::GetDisplayDeviceCount);
    auto fb_result = syscall(hybrid::SyscallNumber::GetFramebufferInfo, reinterpret_cast<uint64_t>(&fb));

    char details[kFastfetchRows][kFastfetchDetailCapacity]{};
    uint64_t detail_count = 0;
    if (system_result.error == hybrid::kSyscallErrorNone) {
        char os_line[64];
        uint64_t os_cursor = 0;
        append_text(os_line, sizeof(os_line), os_cursor, system.sysname);
        append_char(os_line, sizeof(os_line), os_cursor, ' ');
        append_text(os_line, sizeof(os_line), os_cursor, system.release);
        add_fetch_text(details, detail_count, "OS", os_line);
        add_fetch_text(details, detail_count, "Kernel", system.kernel_type);
        add_fetch_text(details, detail_count, "Machine", system.machine);
        add_fetch_text(details, detail_count, "Boot", system.boot_mode);
        add_fetch_hex(details, detail_count, "Boot flags", system.boot_info_flags);
        add_fetch_decimal(details, detail_count, "Modules", system.boot_module_count);
    }
    if (cpu_count.error == hybrid::kSyscallErrorNone) add_fetch_decimal(details, detail_count, "CPUs", cpu_count.value);
    if (memory_result.error == hybrid::kSyscallErrorNone) {
        add_fetch_decimal(details, detail_count, "Memory free KiB", memory.free_pages * 4);
        add_fetch_decimal(details, detail_count, "Memory used KiB", memory.used_pages * 4);
        add_fetch_decimal(details, detail_count, "Usable KiB", memory.usable_bytes / 1024);
    }
    if (fb_result.error == hybrid::kSyscallErrorNone) {
        char geometry[64];
        uint64_t cursor = 0;
        append_decimal(geometry, sizeof(geometry), cursor, fb.width);
        append_char(geometry, sizeof(geometry), cursor, 'x');
        append_decimal(geometry, sizeof(geometry), cursor, fb.height);
        append_char(geometry, sizeof(geometry), cursor, 'x');
        append_decimal(geometry, sizeof(geometry), cursor, fb.bytes_per_pixel * 8);
        add_fetch_text(details, detail_count, "Framebuffer", geometry);
    }
    if (device_count.error == hybrid::kSyscallErrorNone) add_fetch_decimal(details, detail_count, "Devices", device_count.value);
    if (storage_count.error == hybrid::kSyscallErrorNone) add_fetch_decimal(details, detail_count, "Storage", storage_count.value);
    if (network_count.error == hybrid::kSyscallErrorNone) add_fetch_decimal(details, detail_count, "Network", network_count.value);
    if (display_count.error == hybrid::kSyscallErrorNone) add_fetch_decimal(details, detail_count, "Display", display_count.value);
    shell_emit("");
    for (uint64_t i = 0; i < detail_count; ++i) shell_emit(details[i]);
    shell_emit("");
}

void shell_prompt(const char* command) {
    char line[128];
    uint64_t cursor = 0;
    append_text(line, sizeof(line), cursor, "[shell] $ ");
    append_text(line, sizeof(line), cursor, command);
    shell_emit(line);
}

void interactive_prompt() {
    write_terminal(shell_prompt_text);
}

void echo_char(char c) {
    char text[2] = {c, 0};
    write_terminal(text);
}

void terminal_to_bottom() {
    syscall(hybrid::SyscallNumber::TerminalControl, static_cast<uint64_t>(hybrid::TerminalControlCommand::ScrollToBottom));
}

void terminal_scroll(int64_t rows) {
    syscall(hybrid::SyscallNumber::TerminalControl, static_cast<uint64_t>(hybrid::TerminalControlCommand::ScrollRelative), static_cast<uint64_t>(rows));
}

void terminal_reset_input_line() {
    syscall(hybrid::SyscallNumber::TerminalControl, static_cast<uint64_t>(hybrid::TerminalControlCommand::ResetInputLine));
}

void copy_line(char (&target)[kLineCapacity], const char* source) {
    copy_text(target, kLineCapacity, source ? source : "");
}

void redraw_input(char (&line)[kLineCapacity], uint64_t length, uint64_t& cursor, uint64_t target_cursor) {
    if (target_cursor > length) target_cursor = length;
    terminal_reset_input_line();
    interactive_prompt();
    write_terminal(line);
    cursor = length;
    while (cursor > target_cursor) {
        write_terminal("\b");
        --cursor;
    }
}

void replace_input(char (&line)[kLineCapacity], uint64_t& length, uint64_t& cursor, const char* text) {
    copy_text(line, kLineCapacity, text);
    length = strlen(line);
    redraw_input(line, length, cursor, length);
}

void history_push(const char* line) {
    if (!line || line[0] == 0) return;
    uint64_t last_index = shell_history_count == 0 ? kHistoryCapacity : (shell_history_next + kHistoryCapacity - 1) % kHistoryCapacity;
    if (last_index != kHistoryCapacity && streq(shell_history[last_index], line)) return;
    copy_text(shell_history[shell_history_next], kLineCapacity, line);
    shell_history_next = (shell_history_next + 1) % kHistoryCapacity;
    if (shell_history_count < kHistoryCapacity) ++shell_history_count;
}

const char* history_at(uint64_t view) {
    if (view >= shell_history_count) return "";
    uint64_t oldest = shell_history_count == kHistoryCapacity ? shell_history_next : 0;
    return shell_history[(oldest + view) % kHistoryCapacity];
}

void command_help(const char*) {
    shell_line("", "commands: help clear history exit echo status pid ids fgpgid ctx argv env export unset which stat counts spawn jobs fg bg stop usched nextuser uyielddemo upreemptdemo run kill wait reap pwd cd ls cat sh fds ps mem cpus devices fb ticks");
    shell_line("", "external: hello args cat ls uname hostname free meminfo uptime date rtc cal dmesg kmsg loadavg ps processes cmdline procstat pwd env printenv sysinfo fastfetch sysctl id ids groups ctx echo sleep true false touch append rm cp mv dd wc grep tee mkdir rmdir err stat statfs filesystems vfsstat file lsattr namei tree whoami basename dirname head tail test sort uniq find hexdump readelf sha256sum sha224sum sha512sum sha384sum sha1sum md5sum cmp cksum fold printf strings nl tr sed cut paste rev tac seq expr xargs yes od base64 which sh duptest fds lsof fdinh ln readlink realpath truncate blk mount df du lsblk findmnt mountinfo iostat diskstats partitions lsmem iomem bootinfo fbset lspci lsdev devices irqstat interrupts mmstat buddyinfo heapinfo procvmstat netstat route ip ifconfig ethtool lsdrv lsmod pipeinfo pmap maps pcmdline proccomm procenv procwd procexe procroot procfdinfo proclimits procio proctask version limits imginfo abi features kill killall pgrep pidof nproc lscpu cpuinfo schedstat scheddebug vmstat top pstree uyield ubusy slowcat burst loop devio tty ttystat stty ttyread clear");
    shell_line("", "editing: arrows history home end delete tab pageup/pagedown scrollback ctrl-c ctrl-z jobs %n %+ wait -n");
}

void command_clear(const char*) {
    write_terminal("\f");
}

void command_history(const char*) {
    shell_hex("history count ", shell_history_count);
    for (uint64_t i = 0; i < shell_history_count; ++i) {
        shell_path("history ", history_at(i));
    }
}

[[noreturn]] void command_exit(const char*) {
    shell_line("exit ", "0");
    syscall(hybrid::SyscallNumber::Exit, 0);
    for (;;) asm volatile("pause");
}

void command_echo(const char* argument) {
    if (argument && streq(argument, "$?")) {
        shell_decimal("", shell_last_status);
        return;
    }
    shell_line("", argument ? argument : "");
}

void command_status(const char*) {
    shell_decimal("status ", shell_last_status);
}

void command_pid(const char*) {
    auto pid = syscall(hybrid::SyscallNumber::GetCurrentProcessId);
    if (pid.error == hybrid::kSyscallErrorNone) shell_hex("pid current ", pid.value);
    else shell_hex("pid error ", pid.error);
}

void command_ids(const char*) {
    hybrid::CurrentIdsInfo ids;
    auto* bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) bytes[i] = 0;
    auto result = syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("ids error ", result.error);
        return;
    }
    shell_hex("ids pid ", ids.pid);
    shell_hex("ids tid ", ids.tid);
    shell_hex("ids ppid ", ids.parent_pid);
    shell_hex("ids pgid ", ids.process_group_id);
    shell_hex("ids kthread ", ids.kernel_thread_id);
    shell_hex("ids cpu ", ids.cpu_id);
}

uint64_t ensure_shell_process_group() {
    if (shell_process_group_id != 0) return shell_process_group_id;
    hybrid::CurrentIdsInfo ids;
    auto* bytes = reinterpret_cast<unsigned char*>(&ids);
    for (uint64_t i = 0; i < sizeof(ids); ++i) bytes[i] = 0;
    auto result = syscall(hybrid::SyscallNumber::GetCurrentIds, reinterpret_cast<uint64_t>(&ids));
    if (result.error == hybrid::kSyscallErrorNone && ids.process_group_id != 0) shell_process_group_id = ids.process_group_id;
    return shell_process_group_id;
}

bool set_terminal_foreground_group(uint64_t process_group_id, const char* trace_label) {
    uint64_t target = process_group_id == 0 ? ensure_shell_process_group() : process_group_id;
    if (target == 0) return false;
    auto result = syscall(hybrid::SyscallNumber::SetForegroundProcessGroup, target);
    if (result.error != hybrid::kSyscallErrorNone) {
        if (trace_label && shell_trace_commands) shell_hex("fgpgid set error ", result.error);
        return false;
    }
    if (trace_label && shell_trace_commands) shell_hex(trace_label, target);
    return true;
}

void restore_shell_foreground_group() {
    set_terminal_foreground_group(ensure_shell_process_group(), "fgpgid shell ");
}

void command_fgpgid(const char*) {
    auto result = syscall(hybrid::SyscallNumber::GetForegroundProcessGroup);
    if (result.error == hybrid::kSyscallErrorNone) shell_hex("fgpgid ", result.value);
    else shell_hex("fgpgid error ", result.error);
}

void command_ctx(const char*) {
    hybrid::CurrentUserContextInfo context;
    auto result = syscall(hybrid::SyscallNumber::GetCurrentUserContext, reinterpret_cast<uint64_t>(&context));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("ctx error ", result.error);
        return;
    }
    shell_hex("ctx pid ", context.pid);
    shell_hex("ctx tid ", context.tid);
    shell_hex("ctx pstate ", context.process_state);
    shell_hex("ctx tstate ", context.thread_state);
    shell_hex("ctx rip ", context.entry);
    shell_hex("ctx rsp ", context.user_stack_pointer);
    shell_hex("ctx cr3 ", context.address_space_root);
}

void command_argv(const char*) {
    auto count = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        shell_hex("argv error ", count.error);
        return;
    }
    shell_hex("argv count ", count.value);
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ArgumentInfo argument;
        auto result = syscall(hybrid::SyscallNumber::GetArgument, i, reinterpret_cast<uint64_t>(&argument));
        if (result.error == hybrid::kSyscallErrorNone) shell_path("argv ", argument.value);
    }
}

void command_env(const char*) {
    auto count = syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        shell_hex("env error ", count.error);
        return;
    }
    shell_hex("env count ", count.value);
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::EnvironmentInfo environment;
        auto result = syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&environment));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        char line[128];
        uint64_t cursor = 0;
        append_text(line, sizeof(line), cursor, environment.key);
        append_char(line, sizeof(line), cursor, '=');
        append_text(line, sizeof(line), cursor, environment.value);
        shell_path("env ", line);
    }
}

void command_export(const char* argument) {
    if (!argument || argument[0] == 0) {
        command_env("");
        return;
    }
    char key[24];
    char value[80];
    for (uint64_t i = 0; i < sizeof(key); ++i) key[i] = 0;
    for (uint64_t i = 0; i < sizeof(value); ++i) value[i] = 0;
    uint64_t cursor = 0;
    while (argument[cursor] != 0 && argument[cursor] != '=') {
        if (cursor + 1 >= sizeof(key)) {
            shell_line("export: ", "key too long");
            return;
        }
        key[cursor] = argument[cursor];
        ++cursor;
    }
    if (cursor == 0 || argument[cursor] != '=') {
        shell_line("export: ", "expected KEY=value");
        return;
    }
    ++cursor;
    uint64_t out = 0;
    while (argument[cursor] != 0) {
        if (out + 1 >= sizeof(value)) {
            shell_line("export: ", "value too long");
            return;
        }
        value[out++] = argument[cursor++];
    }
    if (out == 0) {
        shell_line("export: ", "empty value");
        return;
    }
    auto result = syscall(hybrid::SyscallNumber::SetEnvironment,
                          reinterpret_cast<uint64_t>(key), strlen(key) + 1,
                          reinterpret_cast<uint64_t>(value), strlen(value) + 1);
    if (result.error == hybrid::kSyscallErrorNone) {
        char line[128];
        uint64_t line_cursor = 0;
        append_text(line, sizeof(line), line_cursor, key);
        append_char(line, sizeof(line), line_cursor, '=');
        append_text(line, sizeof(line), line_cursor, value);
        shell_path("export ", line);
    } else {
        shell_hex("export error ", result.error);
    }
}

void command_unset(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("unset: ", "missing key");
        return;
    }
    auto result = syscall(hybrid::SyscallNumber::UnsetEnvironment,
                          reinterpret_cast<uint64_t>(argument), strlen(argument) + 1);
    if (result.error == hybrid::kSyscallErrorNone) shell_path("unset ", argument);
    else shell_hex("unset error ", result.error);
}

void command_stat(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("stat: ", "missing path");
        return;
    }
    hybrid::VfsStatInfo info;
    auto result = syscall(hybrid::SyscallNumber::VfsStatInfo, reinterpret_cast<uint64_t>(argument), strlen(argument) + 1, reinterpret_cast<uint64_t>(&info));
    if (result.error == hybrid::kSyscallErrorNone) {
        shell_path("stat path ", info.path);
        if (info.type == hybrid::VfsNodeType::Directory) shell_line("stat type ", "directory");
        else if (info.type == hybrid::VfsNodeType::MemoryFile) shell_line("stat type ", "memory-file");
        else if (info.type == hybrid::VfsNodeType::CharacterDevice) shell_line("stat type ", "char-device");
        else if (info.type == hybrid::VfsNodeType::VirtualFile) shell_line("stat type ", "virtual-file");
        else shell_line("stat type ", "unknown");
        shell_hex("stat size ", info.size);
        shell_hex("stat flags ", info.flags);
    } else {
        shell_hex("stat error ", result.error);
    }
}

void command_counts(const char*) {
    auto processes = syscall(hybrid::SyscallNumber::GetProcessCount);
    auto live = syscall(hybrid::SyscallNumber::GetLiveProcessCount);
    auto exited = syscall(hybrid::SyscallNumber::GetExitedProcessCount);
    auto user_threads = syscall(hybrid::SyscallNumber::GetUserThreadCount);
    auto runnable_threads = syscall(hybrid::SyscallNumber::GetRunnableUserThreadCount);
    if (processes.error == hybrid::kSyscallErrorNone) shell_hex("counts processes ", processes.value);
    if (live.error == hybrid::kSyscallErrorNone) shell_hex("counts live ", live.value);
    if (exited.error == hybrid::kSyscallErrorNone) shell_hex("counts exited ", exited.value);
    if (user_threads.error == hybrid::kSyscallErrorNone) shell_hex("counts user threads ", user_threads.value);
    if (runnable_threads.error == hybrid::kSyscallErrorNone) shell_hex("counts runnable threads ", runnable_threads.value);
}

void command_spawn(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("spawn: ", "missing path");
        return;
    }
    uint64_t pid = 0;
    auto result = syscall(hybrid::SyscallNumber::Spawn,
                          reinterpret_cast<uint64_t>(argument),
                          strlen(argument) + 1,
                          reinterpret_cast<uint64_t>(&pid),
                          hybrid::SpawnFlagStartSuspended);
    if (pid != 0) {
        shell_hex("spawn pid ", pid);
        last_spawn_pid = pid;
        remember_job(pid, argument);
    }
    else shell_hex("spawn error ", result.error);
}

void command_jobs(const char*) {
    uint64_t active = 0;
    for (uint64_t i = 0; i < kJobCapacity; ++i) if (shell_jobs[i].active) ++active;
    shell_hex("jobs count ", active);
    for (uint64_t i = 0; i < kJobCapacity; ++i) {
        ShellJob& job = shell_jobs[i];
        if (!job.active) continue;
        hybrid::ProcessInfo info;
        bool found = process_info_by_pid(job.pid, info);
        char line[256];
        uint64_t cursor = 0;
        append_text(line, sizeof(line), cursor, "job %");
        append_decimal(line, sizeof(line), cursor, job.job_id);
        append_text(line, sizeof(line), cursor, " pid=");
        append_hex(line, sizeof(line), cursor, job.pid);
        append_text(line, sizeof(line), cursor, " pgid=");
        append_hex(line, sizeof(line), cursor, job.process_group_id);
        append_text(line, sizeof(line), cursor, " procs=");
        append_hex(line, sizeof(line), cursor, job.process_count);
        append_text(line, sizeof(line), cursor, " state=");
        append_text(line, sizeof(line), cursor, found ? process_state_name(info.state) : "reaped");
        append_text(line, sizeof(line), cursor, " reason=");
        append_text(line, sizeof(line), cursor, found ? process_reason_name(info.termination_reason) : "none");
        append_text(line, sizeof(line), cursor, " code=");
        append_hex(line, sizeof(line), cursor, found ? info.exit_code : job.last_status);
        append_text(line, sizeof(line), cursor, " cmd=");
        append_text(line, sizeof(line), cursor, job.command);
        shell_line("", line);
        for (uint64_t member = 0; member < job.process_count && member < kMaxJobProcesses; ++member) {
            shell_hex("job member pid ", job.pids[member]);
        }
    }
}

bool wait_job_processes(ShellJob& job, bool poll_interrupt, uint64_t& final_status, uint64_t& total_polls) {
    final_status = 0;
    total_polls = 0;
    uint64_t count = job.process_count == 0 ? 1 : job.process_count;
    if (count > kMaxJobProcesses) count = kMaxJobProcesses;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t pid = job.process_count == 0 ? job.pid : job.pids[i];
        if (pid == 0) continue;
        uint64_t code = 0;
        uint64_t polls = 0;
        for (;;) {
            auto waited = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
            if (waited.error == hybrid::kSyscallErrorNone) break;
            if (waited.error != hybrid::kSyscallErrorWouldBlock) {
                shell_hex("wait job error ", waited.error);
                return false;
            }
            ++polls;
            if (poll_interrupt) poll_job_control_for_process_group(job.process_group_id);
            syscall(hybrid::SyscallNumber::Yield);
        }
        shell_hex("wait job pid ", pid);
        shell_hex("wait job code ", code);
        total_polls += polls;
        final_status = code;
    }
    job.last_status = final_status;
    job.waited = true;
    return true;
}

bool reap_job_processes(ShellJob& job) {
    bool any = false;
    uint64_t count = job.process_count == 0 ? 1 : job.process_count;
    if (count > kMaxJobProcesses) count = kMaxJobProcesses;
    for (uint64_t i = 0; i < count; ++i) {
        uint64_t pid = job.process_count == 0 ? job.pid : job.pids[i];
        if (pid == 0) continue;
        auto reaped = syscall(hybrid::SyscallNumber::ReapProcess, pid);
        if (reaped.error == hybrid::kSyscallErrorNone && reaped.value != 0) {
            shell_hex("reap job pid ", pid);
            any = true;
        } else {
            shell_hex("reap job error ", reaped.error);
        }
    }
    if (any) forget_job(job.pid);
    return any;
}

void command_fg(const char* argument) {
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("fg: ", "missing pid");
        return;
    }
    ShellJob* job = find_job(pid);
    if (!job) {
        shell_line("fg: ", "job not tracked");
        return;
    }
    shell_decimal("fg job ", job->job_id);
    shell_hex("fg pid ", pid);
    shell_hex("fg pgid ", job->process_group_id);
    auto continued = syscall(hybrid::SyscallNumber::ContinueProcessGroup, job->process_group_id);
    if (continued.error == hybrid::kSyscallErrorNone) shell_hex("fg continued ", continued.value);
    set_terminal_foreground_group(job->process_group_id, "fgpgid fg ");
    uint64_t code = 0;
    uint64_t polls = 0;
    if (!wait_job_processes(*job, true, code, polls)) code = 126;
    restore_shell_foreground_group();
    shell_last_status = code;
    shell_hex("fg exit ", code);
    shell_hex("fg wait polls ", polls);
    reap_job_processes(*job);
}

void command_bg(const char* argument) {
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("bg: ", "missing pid");
        return;
    }
    ShellJob* job = find_job(pid);
    if (!job || job->process_group_id == 0) {
        shell_line("bg: ", "job not tracked");
        return;
    }
    auto continued = syscall(hybrid::SyscallNumber::ContinueProcessGroup, job->process_group_id);
    if (continued.error != hybrid::kSyscallErrorNone) {
        shell_hex("bg continue error ", continued.error);
        return;
    }
    shell_decimal("bg job ", job->job_id);
    shell_hex("bg pgid ", job->process_group_id);
    shell_hex("bg continued ", continued.value);
    shell_line("bg ", "running");
    shell_last_status = 0;
}

void command_stop(const char* argument) {
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("stop: ", "missing pid");
        return;
    }
    ShellJob* job = find_job(pid);
    if (!job || job->process_group_id == 0) {
        shell_line("stop: ", "job not tracked");
        return;
    }
    auto stopped = syscall(hybrid::SyscallNumber::StopProcessGroup, job->process_group_id);
    if (stopped.error != hybrid::kSyscallErrorNone) {
        shell_hex("stop error ", stopped.error);
        return;
    }
    shell_decimal("stop job ", job->job_id);
    shell_hex("stop pgid ", job->process_group_id);
    shell_hex("stop count ", stopped.value);
    shell_last_status = 0;
}

uint64_t command_pid_or_last(const char* argument) {
    if (!argument || argument[0] == 0) return last_spawn_pid;
    if (argument[0] == '%') {
        if (argument[1] == '+' && argument[2] == 0) {
            ShellJob* job = current_job();
            return job ? job->pid : 0;
        }
        bool ok = false;
        uint64_t job_id = parse_number(argument + 1, ok);
        ShellJob* job = ok ? find_job_id(job_id) : nullptr;
        return job ? job->pid : 0;
    }
    bool ok = false;
    uint64_t pid = parse_number(argument, ok);
    return ok ? pid : 0;
}

uint64_t parse_kill_signal(const char* text, bool& ok) {
    ok = false;
    if (!text || text[0] == 0) {
        ok = true;
        return static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    }
    if (text[0] == '-' && text[1] == '9' && text[2] == 0) {
        ok = true;
        return static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill);
    }
    if (text[0] == '-' && text[1] == '1' && text[2] == '5' && text[3] == 0) {
        ok = true;
        return static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    }
    if ((text[0] == 'K' || text[0] == 'k') && (text[1] == 'I' || text[1] == 'i') &&
        (text[2] == 'L' || text[2] == 'l') && (text[3] == 'L' || text[3] == 'l') && text[4] == 0) {
        ok = true;
        return static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill);
    }
    if ((text[0] == 'T' || text[0] == 't') && (text[1] == 'E' || text[1] == 'e') &&
        (text[2] == 'R' || text[2] == 'r') && (text[3] == 'M' || text[3] == 'm') && text[4] == 0) {
        ok = true;
        return static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    }
    return 0;
}

void command_kill(const char* argument) {
    char first[32];
    char second[32];
    copy_text(first, sizeof(first), "");
    copy_text(second, sizeof(second), "");
    const char* cursor = argument ? argument : "";
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    uint64_t out = 0;
    while (*cursor != 0 && *cursor != ' ' && *cursor != '\t' && out + 1 < sizeof(first)) first[out++] = *cursor++;
    first[out] = 0;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    out = 0;
    while (*cursor != 0 && *cursor != ' ' && *cursor != '\t' && out + 1 < sizeof(second)) second[out++] = *cursor++;
    second[out] = 0;

    bool signal_ok = false;
    uint64_t signal = static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm);
    const char* pid_text = first;
    if (first[0] == '-' || first[0] == 'K' || first[0] == 'k' || first[0] == 'T' || first[0] == 't') {
        signal = parse_kill_signal(first, signal_ok);
        if (!signal_ok) {
            shell_line("kill: ", "unknown signal");
            return;
        }
        pid_text = second[0] == 0 ? "" : second;
    } else if (second[0] != 0) {
        signal = parse_kill_signal(second, signal_ok);
        if (!signal_ok) {
            shell_line("kill: ", "unknown signal");
            return;
        }
    }
    uint64_t pid = command_pid_or_last(pid_text);
    if (pid == 0) {
        shell_line("kill: ", "missing pid");
        return;
    }
    if (ShellJob* job = find_job(pid)) {
        if (job->process_count > 1 && job->process_group_id != 0) {
            auto killed = syscall(hybrid::SyscallNumber::KillProcessGroup, job->process_group_id, signal);
            if (killed.error == hybrid::kSyscallErrorNone) {
                shell_decimal("kill job ", job->job_id);
                shell_hex("kill pgid ", job->process_group_id);
                shell_hex("kill count ", killed.value);
                shell_line("kill reason ", signal == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill) ? "sigkill" : "sigterm");
            } else {
                shell_hex("kill group error ", killed.error);
            }
            return;
        }
    }
    auto result = syscall(hybrid::SyscallNumber::Kill, pid, signal);
    if (result.error == hybrid::kSyscallErrorNone) {
        if (ShellJob* job = find_job(pid)) shell_decimal("kill job ", job->job_id);
        shell_hex("kill pid ", pid);
        shell_line("kill reason ", signal == static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigKill) ? "sigkill" : "sigterm");
    }
    else shell_hex("kill error ", result.error);
}

bool poll_job_control_for_process_group(uint64_t process_group_id) {
    if (process_group_id == 0) return false;
    char input = 0;
    auto key = syscall(hybrid::SyscallNumber::Read, hybrid::kStdinFd, reinterpret_cast<uint64_t>(&input), 1);
    if (key.error != hybrid::kSyscallErrorNone || key.value == 0) return false;
    if (input == 0x03) {
        auto killed = syscall(hybrid::SyscallNumber::KillProcessGroup, process_group_id, static_cast<uint64_t>(hybrid::ProcessTerminationReason::SigTerm));
        if (killed.error == hybrid::kSyscallErrorNone) {
            shell_hex("interrupt pgid ", process_group_id);
            shell_hex("interrupt killed ", killed.value);
            shell_line("interrupt reason ", "sigterm");
            return true;
        }
        shell_hex("interrupt group error ", killed.error);
        return false;
    }
    if (input == 0x1a) {
        auto stopped = syscall(hybrid::SyscallNumber::StopProcessGroup, process_group_id);
        if (stopped.error == hybrid::kSyscallErrorNone) {
            shell_hex("suspend pgid ", process_group_id);
            shell_hex("suspend stopped ", stopped.value);
            if (ShellJob* job = find_job(process_group_id)) job->waited = false;
            return true;
        }
        shell_hex("suspend group error ", stopped.error);
    }
    return false;
}

void command_wait(const char* argument) {
    if (argument && (streq(argument, "-a") || streq(argument, "-n"))) {
        shell_line("wait mode ", streq(argument, "-n") ? "any-next" : "any");
        hybrid::WaitAnyInfo info;
        auto* bytes = reinterpret_cast<unsigned char*>(&info);
        for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
        hybrid::SyscallResult result;
        result.value = 0;
        result.error = hybrid::kSyscallErrorNone;
        uint64_t polls = 0;
        for (;;) {
            result = syscall(hybrid::SyscallNumber::WaitAny, reinterpret_cast<uint64_t>(&info));
            if (result.error != hybrid::kSyscallErrorWouldBlock) break;
            ++polls;
            syscall(hybrid::SyscallNumber::Yield);
        }
        if (result.error == hybrid::kSyscallErrorNone) {
            if (ShellJob* job = find_job(info.pid)) {
                job->last_status = info.exit_code;
                job->waited = true;
            }
            shell_hex("wait any pid ", info.pid);
            shell_hex("wait any code ", info.exit_code);
            shell_hex("wait any polls ", polls);
            shell_last_status = info.exit_code;
        } else {
            shell_hex("wait any error ", result.error);
        }
        return;
    }
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("wait: ", "missing pid");
        return;
    }
    if (ShellJob* job = find_job(pid)) {
        uint64_t code = 0;
        uint64_t polls = 0;
        if (wait_job_processes(*job, false, code, polls)) {
            shell_hex("wait code ", code);
            shell_hex("wait polls ", polls);
            shell_last_status = code;
        }
        return;
    }
    uint64_t code = 0;
    auto result = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
    if (result.error == hybrid::kSyscallErrorNone) {
        if (ShellJob* job = find_job(pid)) {
            job->last_status = code;
            job->waited = true;
        }
        shell_hex("wait code ", code);
    }
    else shell_hex("wait error ", result.error);
}

void command_reap(const char* argument) {
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("reap: ", "missing pid");
        return;
    }
    if (ShellJob* job = find_job(pid)) {
        if (job->process_count > 1) {
            shell_decimal("reap job ", job->job_id);
            reap_job_processes(*job);
            return;
        }
    }
    auto result = syscall(hybrid::SyscallNumber::ReapProcess, pid);
    if (result.error == hybrid::kSyscallErrorNone && result.value != 0) {
        if (ShellJob* job = find_job(pid)) shell_decimal("reap job ", job->job_id);
        shell_hex("reap pid ", pid);
        forget_job(pid);
    }
    else shell_hex("reap error ", result.error);
}

void command_usched(const char*) {
    hybrid::UserSchedulerInfo info;
    auto result = syscall(hybrid::SyscallNumber::GetUserSchedulerInfo, reinterpret_cast<uint64_t>(&info));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("usched error ", result.error);
        return;
    }
    shell_hex("usched current tid ", info.current_tid);
    shell_hex("usched current pid ", info.current_pid);
    shell_hex("usched runnable ", info.runnable_threads);
    shell_hex("usched running ", info.running_threads);
    shell_hex("usched last selected ", info.last_selected_tid);
    shell_hex("usched schedulable ", info.schedulable_threads);
    shell_hex("usched quantum ", info.timeslice_quantum);
    shell_hex("usched slice ticks ", info.current_slice_ticks);
    shell_hex("usched expired slices ", info.expired_slices);
}

void command_nextuser(const char*) {
    hybrid::LaunchContextInfo context;
    auto result = syscall(hybrid::SyscallNumber::SelectNextUserThread, reinterpret_cast<uint64_t>(&context));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("nextuser error ", result.error);
        return;
    }
    shell_hex("nextuser tid ", context.tid);
    shell_hex("nextuser pid ", context.pid);
    shell_hex("nextuser rip ", context.rip);
}

void command_uyielddemo(const char*) {
    uint64_t pid = 0;
    static const char command_line[] = "/bin/uyield.elf";
    auto spawned = syscall(hybrid::SyscallNumber::Spawn,
                           reinterpret_cast<uint64_t>(command_line),
                           sizeof(command_line),
                           reinterpret_cast<uint64_t>(&pid));
    if (spawned.error != hybrid::kSyscallErrorNone || pid == 0) {
        shell_hex("uyield spawn error ", spawned.error);
        shell_last_status = 126;
        return;
    }
    last_spawn_pid = pid;
    shell_hex("uyield child pid ", pid);
    syscall(hybrid::SyscallNumber::Yield);
    shell_line("uyield ", "parent resumed");
    syscall(hybrid::SyscallNumber::Yield);
    uint64_t code = 0;
    auto waited = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
    if (waited.error == hybrid::kSyscallErrorNone) {
        shell_hex("uyield wait code ", code);
        shell_last_status = code;
    } else {
        shell_hex("uyield wait error ", waited.error);
        shell_last_status = 126;
    }
    auto reaped = syscall(hybrid::SyscallNumber::ReapProcess, pid);
    if (reaped.error == hybrid::kSyscallErrorNone && reaped.value != 0) shell_hex("uyield reap pid ", pid);
    else shell_hex("uyield reap error ", reaped.error);
}

void command_upreemptdemo(const char*) {
    uint64_t pid = 0;
    static const char command_line[] = "/bin/ubusy.elf";
    auto spawned = syscall(hybrid::SyscallNumber::Spawn,
                           reinterpret_cast<uint64_t>(command_line),
                           sizeof(command_line),
                           reinterpret_cast<uint64_t>(&pid),
                           hybrid::SpawnFlagStartSuspended);
    if (spawned.error != hybrid::kSyscallErrorNone || pid == 0) {
        shell_hex("upreempt spawn error ", spawned.error);
        shell_last_status = 126;
        return;
    }
    last_spawn_pid = pid;
    shell_hex("upreempt child pid ", pid);
    auto started = syscall(hybrid::SyscallNumber::StartProcess, pid);
    if (started.error != hybrid::kSyscallErrorNone) {
        shell_hex("upreempt start error ", started.error);
        shell_last_status = 126;
        return;
    }
    uint64_t code = 0;
    uint64_t polls = 0;
    for (;;) {
        auto waited = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        ++polls;
        asm volatile("pause" : : : "memory");
    }
    shell_hex("upreempt polls ", polls);
    shell_hex("upreempt wait code ", code);
    shell_last_status = code;
    auto reaped = syscall(hybrid::SyscallNumber::ReapProcess, pid);
    if (reaped.error == hybrid::kSyscallErrorNone && reaped.value != 0) shell_hex("upreempt reap pid ", pid);
    else shell_hex("upreempt reap error ", reaped.error);
}

void command_run(const char* argument) {
    uint64_t pid = command_pid_or_last(argument);
    if (pid == 0) {
        shell_line("run: ", "missing pid");
        return;
    }
    hybrid::ProcessInfo info;
    if (process_info_by_pid(pid, info) && info.state == 1) {
        shell_hex("run start pid ", pid);
        auto started = syscall(hybrid::SyscallNumber::StartProcess, pid);
        if (started.error != hybrid::kSyscallErrorNone) {
            shell_hex("run start error ", started.error);
            shell_last_status = 126;
            return;
        }
    }
    uint64_t code = 0;
    uint64_t polls = 0;
    for (;;) {
        auto waited = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        ++polls;
        poll_job_control_for_process_group(pid);
        syscall(hybrid::SyscallNumber::Yield);
    }
    shell_last_status = code;
    shell_hex("run exit ", code);
    shell_hex("run wait polls ", polls);
}

void command_pwd(const char*) {
    hybrid::PathInfo cwd;
    auto result = syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
    if (result.error == hybrid::kSyscallErrorNone) shell_path("cwd ", cwd.path);
    else shell_hex("pwd error ", result.error);
}

void command_cd(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("cd: ", "missing path");
        return;
    }
    auto result = syscall(hybrid::SyscallNumber::SetCurrentDirectory, reinterpret_cast<uint64_t>(argument), strlen(argument) + 1);
    if (result.error == hybrid::kSyscallErrorNone) shell_path("cd ", argument);
    else shell_hex("cd error ", result.error);
}

void command_ls(const char* argument) {
    char target[64];
    if (argument && argument[0] != 0) {
        copy_text(target, sizeof(target), argument);
    } else {
        hybrid::PathInfo cwd;
        auto cwd_result = syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
        copy_text(target, sizeof(target), cwd_result.error == hybrid::kSyscallErrorNone ? cwd.path : "/");
    }

    hybrid::VfsStatInfo stat;
    auto stat_result = syscall(hybrid::SyscallNumber::VfsStatInfo,
                               reinterpret_cast<uint64_t>(target),
                               strlen(target) + 1,
                               reinterpret_cast<uint64_t>(&stat));
    if (stat_result.error != hybrid::kSyscallErrorNone || stat_result.value != 1) {
        shell_hex("ls error ", stat_result.error);
        return;
    }
    if (stat.type != hybrid::VfsNodeType::Directory) {
        shell_path("file ", stat.path);
        return;
    }
    constexpr uint64_t kShellLsMaxEntries = 256;
    for (uint64_t i = 0; i < kShellLsMaxEntries; ++i) {
        hybrid::VfsDirectoryEntryInfo entry;
        auto result = syscall(hybrid::SyscallNumber::ReadDirectory,
                              reinterpret_cast<uint64_t>(target),
                              strlen(target) + 1,
                              i,
                              reinterpret_cast<uint64_t>(&entry));
        if (result.error != hybrid::kSyscallErrorNone || result.value != 1) break;
        shell_path(entry.type == hybrid::VfsNodeType::Directory ? "dir " :
                   (entry.type == hybrid::VfsNodeType::CharacterDevice ? "char " :
                   (entry.type == hybrid::VfsNodeType::VirtualFile ? "virt " : "file ")), entry.path);
    }
}

void command_cat(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("cat: ", "missing path");
        return;
    }
    auto fd = syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(argument), strlen(argument) + 1);
    if (fd.error != hybrid::kSyscallErrorNone || fd.value < 3) {
        shell_hex("cat open error ", fd.error);
        return;
    }
    char bytes[16];
    auto read = syscall(hybrid::SyscallNumber::Read, fd.value, reinterpret_cast<uint64_t>(bytes), sizeof(bytes));
    if (read.error == hybrid::kSyscallErrorNone) {
        shell_hex("cat bytes ", read.value);
        if (read.value >= 4) {
            uint64_t magic = static_cast<uint8_t>(bytes[0]) |
                             (static_cast<uint64_t>(static_cast<uint8_t>(bytes[1])) << 8) |
                             (static_cast<uint64_t>(static_cast<uint8_t>(bytes[2])) << 16) |
                             (static_cast<uint64_t>(static_cast<uint8_t>(bytes[3])) << 24);
            shell_hex("cat first32 ", magic);
        }
    } else {
        shell_hex("cat read error ", read.error);
    }
    syscall(hybrid::SyscallNumber::Close, fd.value);
}

void command_sh(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("sh: ", "missing script");
        shell_last_status = 1;
        return;
    }
    auto fd = syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(argument), strlen(argument) + 1);
    if (fd.error != hybrid::kSyscallErrorNone || fd.value < 3) {
        shell_hex("sh open error ", fd.error);
        shell_last_status = 2;
        return;
    }
    shell_path("sh script ", argument);
    char line[kLineCapacity];
    uint64_t line_len = 0;
    char c = 0;
    uint64_t lines = 0;
    for (;;) {
        auto read = syscall(hybrid::SyscallNumber::Read, fd.value, reinterpret_cast<uint64_t>(&c), 1);
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        if (c == '\r') continue;
        if (c == '\n') {
            line[line_len] = 0;
            if (line_len != 0 && line[0] != '#') {
                run_shell_line(line);
                ++lines;
            }
            line_len = 0;
        } else if (line_len + 1 < sizeof(line)) {
            line[line_len++] = c;
        }
    }
    if (line_len != 0 && line[0] != '#') {
        line[line_len] = 0;
        run_shell_line(line);
        ++lines;
    }
    syscall(hybrid::SyscallNumber::Close, fd.value);
    shell_hex("sh lines ", lines);
}

const char* fd_kind_name(hybrid::FileDescriptorInfoKind kind) {
    switch (kind) {
    case hybrid::FileDescriptorInfoKind::Vfs: return "vfs";
    case hybrid::FileDescriptorInfoKind::PipeRead: return "pipe-read";
    case hybrid::FileDescriptorInfoKind::PipeWrite: return "pipe-write";
    default: return "empty";
    }
}

void command_fds(const char* argument) {
    uint64_t pid = 0;
    if (argument && argument[0] != 0) {
        bool ok = false;
        pid = parse_number(argument, ok);
        if (!ok) {
            shell_line("fds: ", "bad pid");
            return;
        }
    }
    uint64_t count = 0;
    shell_hex("fds pid ", pid);
    for (uint64_t i = 0; i < 8; ++i) {
        hybrid::FileDescriptorInfo info;
        auto result = syscall(hybrid::SyscallNumber::GetFileDescriptorInfo, pid, i, reinterpret_cast<uint64_t>(&info));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        ++count;
        shell_hex("fds fd ", info.fd);
        shell_path("fds kind ", fd_kind_name(info.kind));
        if (info.kind == hybrid::FileDescriptorInfoKind::Vfs) {
            shell_hex("fds handle ", info.vfs_handle);
            shell_hex("fds offset ", info.offset);
            shell_path("fds path ", info.path);
        } else if (info.kind == hybrid::FileDescriptorInfoKind::PipeRead || info.kind == hybrid::FileDescriptorInfoKind::PipeWrite) {
            shell_hex("fds pipe ", info.pipe_id);
        }
    }
    shell_hex("fds count ", count);
}

void command_ps(const char*) {
    auto count = syscall(hybrid::SyscallNumber::GetProcessCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        shell_hex("ps error ", count.error);
        return;
    }
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ProcessInfo process;
        auto result = syscall(hybrid::SyscallNumber::GetProcessInfo, i, reinterpret_cast<uint64_t>(&process));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        shell_hex("pid ", process.pid);
        shell_hex("ppid ", process.parent_pid);
        shell_hex("state ", process.state);
        shell_path("proc ", process.name);
    }
    auto threads = syscall(hybrid::SyscallNumber::GetUserThreadCount);
    if (threads.error != hybrid::kSyscallErrorNone) return;
    shell_hex("threads ", threads.value);
    for (uint64_t i = 0; i < threads.value; ++i) {
        hybrid::UserThreadInfo thread;
        auto result = syscall(hybrid::SyscallNumber::GetUserThreadInfo, i, reinterpret_cast<uint64_t>(&thread));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        shell_hex("tid ", thread.tid);
        shell_hex("tpid ", thread.pid);
        shell_hex("tstate ", thread.state);
        shell_hex("tblock ", thread.block_reason);
        if (thread.wait_pipe_id != 0) shell_hex("twait pipe ", thread.wait_pipe_id);
        if (thread.wait_process_id != 0) shell_hex("twait pid ", thread.wait_process_id);
    }
}

void command_mem(const char*) {
    hybrid::MemoryStatsInfo stats;
    auto result = syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&stats));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("mem error ", result.error);
        return;
    }
    shell_hex("mem total pages ", stats.total_pages);
    shell_hex("mem free pages ", stats.free_pages);
    shell_hex("mem used pages ", stats.used_pages);
}

void command_cpus(const char*) {
    auto count = syscall(hybrid::SyscallNumber::GetCpuCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        shell_hex("cpus error ", count.error);
        return;
    }
    shell_hex("cpus count ", count.value);
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::CpuInfo cpu;
        auto result = syscall(hybrid::SyscallNumber::GetCpuInfo, i, reinterpret_cast<uint64_t>(&cpu));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        shell_hex("cpu id ", cpu.cpu_id);
        shell_hex("cpu apic ", cpu.apic_id);
        shell_hex("cpu acpi ", cpu.acpi_processor_id);
        shell_hex("cpu flags ", cpu.flags);
    }
}

void command_devices(const char*) {
    auto count = syscall(hybrid::SyscallNumber::GetDeviceCount);
    if (count.error != hybrid::kSyscallErrorNone) {
        shell_hex("devices error ", count.error);
        return;
    }
    shell_hex("devices count ", count.value);
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::DeviceInfo device;
        auto result = syscall(hybrid::SyscallNumber::GetDeviceInfo, i, reinterpret_cast<uint64_t>(&device));
        if (result.error != hybrid::kSyscallErrorNone) continue;
        shell_hex("device vendor ", device.vendor_id);
        shell_hex("device id ", device.device_id);
    }
}

void command_fb(const char*) {
    hybrid::FramebufferInfo fb;
    auto result = syscall(hybrid::SyscallNumber::GetFramebufferInfo, reinterpret_cast<uint64_t>(&fb));
    if (result.error != hybrid::kSyscallErrorNone) {
        shell_hex("fb error ", result.error);
        return;
    }
    shell_hex("fb width ", fb.width);
    shell_hex("fb height ", fb.height);
    shell_hex("fb scanline ", fb.pixels_per_scanline);
    shell_hex("fb bytespp ", fb.bytes_per_pixel);
}

void command_ticks(const char*) {
    auto ticks = syscall(hybrid::SyscallNumber::GetTicks);
    if (ticks.error == hybrid::kSyscallErrorNone) shell_hex("ticks ", ticks.value);
    else shell_hex("ticks error ", ticks.error);
}

bool environment_value(const char* key, char* out, uint64_t capacity) {
    if (!key || !out || capacity == 0) return false;
    out[0] = 0;
    auto count = syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::EnvironmentInfo environment;
        auto result = syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&environment));
        if (result.error != hybrid::kSyscallErrorNone || !streq(environment.key, key)) continue;
        copy_text(out, capacity, environment.value);
        return out[0] != 0;
    }
    return false;
}

bool variable_name_char(char c, bool first) {
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool digit = c >= '0' && c <= '9';
    return first ? alpha : (alpha || digit);
}

void append_environment_value(char* out, uint64_t capacity, uint64_t& cursor, const char* key, uint64_t key_length) {
    if (!key || key_length == 0) return;
    char name[24];
    if (key_length >= sizeof(name)) return;
    for (uint64_t i = 0; i < key_length; ++i) name[i] = key[i];
    name[key_length] = 0;
    char value[80];
    if (environment_value(name, value, sizeof(value))) append_text(out, capacity, cursor, value);
}

void expand_shell_variables(const char* input, char* out, uint64_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = 0;
    if (!input) return;
    uint64_t cursor = 0;
    for (uint64_t i = 0; input[i] != 0; ++i) {
        if (input[i] != '$') {
            append_char(out, capacity, cursor, input[i]);
            continue;
        }
        char next = input[i + 1];
        if (next == '?') {
            append_decimal(out, capacity, cursor, shell_last_status);
            ++i;
            continue;
        }
        if (next == '{') {
            uint64_t start = i + 2;
            uint64_t end = start;
            while (input[end] != 0 && input[end] != '}') ++end;
            if (input[end] == '}') {
                append_environment_value(out, capacity, cursor, input + start, end - start);
                i = end;
                continue;
            }
        }
        if (variable_name_char(next, true)) {
            uint64_t start = i + 1;
            uint64_t end = start + 1;
            while (variable_name_char(input[end], false)) ++end;
            append_environment_value(out, capacity, cursor, input + start, end - start);
            i = end - 1;
            continue;
        }
        append_char(out, capacity, cursor, '$');
    }
}

bool has_slash(const char* text) {
    if (!text) return false;
    for (uint64_t i = 0; text[i] != 0; ++i) {
        if (text[i] == '/') return true;
    }
    return false;
}

bool path_exists(const char* path) {
    if (!path || path[0] == 0) return false;
    auto stat = syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(path), strlen(path) + 1);
    return stat.error == hybrid::kSyscallErrorNone;
}

bool resolve_external_path(const char* name, char (&out)[64]) {
    if (!name || name[0] == 0) return false;
    if (has_slash(name)) {
        copy_text(out, sizeof(out), name);
        return path_exists(out);
    }

    char path_env[80];
    if (!environment_value("PATH", path_env, sizeof(path_env))) copy_text(path_env, sizeof(path_env), "/bin");
    uint64_t cursor = 0;
    while (path_env[cursor] != 0) {
        while (path_env[cursor] == ':') ++cursor;
        if (path_env[cursor] == 0) break;
        char candidate[64];
        uint64_t candidate_cursor = 0;
        while (path_env[cursor] != 0 && path_env[cursor] != ':') {
            append_char(candidate, sizeof(candidate), candidate_cursor, path_env[cursor++]);
        }
        if (candidate_cursor == 0) continue;
        if (candidate[candidate_cursor - 1] != '/') append_char(candidate, sizeof(candidate), candidate_cursor, '/');
        append_text(candidate, sizeof(candidate), candidate_cursor, name);
        append_text(candidate, sizeof(candidate), candidate_cursor, ".elf");
        if (path_exists(candidate)) {
            copy_text(out, sizeof(out), candidate);
            return true;
        }
    }
    return false;
}

void command_which(const char* argument) {
    if (!argument || argument[0] == 0) {
        shell_line("which: ", "missing command");
        return;
    }
    char resolved[64];
    if (resolve_external_path(argument, resolved)) shell_path("which ", resolved);
    else shell_path("which missing ", argument);
}

struct ShellCommand {
    const char* name;
    void (*handler)(const char*);
};

const ShellCommand kCommands[] = {
    {"help", command_help},
    {"clear", command_clear},
    {"history", command_history},
    {"exit", command_exit},
    {"echo", command_echo},
    {"status", command_status},
    {"pid", command_pid},
    {"ids", command_ids},
    {"fgpgid", command_fgpgid},
    {"ctx", command_ctx},
    {"argv", command_argv},
    {"env", command_env},
    {"export", command_export},
    {"unset", command_unset},
    {"which", command_which},
    {"stat", command_stat},
    {"counts", command_counts},
    {"spawn", command_spawn},
    {"jobs", command_jobs},
    {"fg", command_fg},
    {"bg", command_bg},
    {"stop", command_stop},
    {"usched", command_usched},
    {"nextuser", command_nextuser},
    {"uyielddemo", command_uyielddemo},
    {"upreemptdemo", command_upreemptdemo},
    {"run", command_run},
    {"kill", command_kill},
    {"wait", command_wait},
    {"reap", command_reap},
    {"pwd", command_pwd},
    {"cd", command_cd},
    {"ls", command_ls},
    {"cat", command_cat},
    {"sh", command_sh},
    {"fds", command_fds},
    {"ps", command_ps},
    {"mem", command_mem},
    {"cpus", command_cpus},
    {"devices", command_devices},
    {"fb", command_fb},
    {"ticks", command_ticks},
};

const char* const kExternalCommands[] = {
    "hello", "args", "cat", "ls", "uname", "hostname", "free", "meminfo", "uptime", "date", "rtc", "cal", "dmesg", "kmsg", "loadavg", "ps", "processes", "cmdline", "procstat", "pwd", "env",
    "sysinfo", "fastfetch", "sysctl", "id", "ids", "groups", "ctx", "echo", "sleep", "true", "false", "touch", "append", "rm", "cp", "mv", "dd", "wc", "grep", "tee", "mkdir", "rmdir", "err", "printenv",
    "stat", "statfs", "filesystems", "vfsstat", "file", "lsattr", "namei", "tree", "whoami", "basename", "dirname", "head", "tail", "test", "sort", "uniq", "find", "hexdump", "readelf", "sha256sum", "sha224sum", "sha512sum", "sha384sum", "sha1sum", "md5sum", "cmp", "cksum", "fold", "printf", "strings", "nl", "tr", "sed", "cut", "paste", "rev", "tac", "seq", "expr", "xargs", "yes", "od", "base64", "which", "sh", "duptest", "fds", "lsof", "fdinh", "ln", "readlink", "realpath", "truncate", "blk", "mount", "df", "du", "lsblk", "findmnt", "mountinfo", "iostat", "diskstats", "partitions", "lsmem", "iomem", "bootinfo", "fbset", "lspci", "lsdev", "devices", "irqstat", "interrupts", "mmstat", "buddyinfo", "heapinfo", "procvmstat", "netstat", "route", "ip", "ifconfig", "ethtool", "lsdrv", "lsmod", "pipeinfo", "pmap", "maps", "pcmdline", "proccomm", "procenv", "procwd", "procexe", "procroot", "procfdinfo", "proclimits", "procio", "proctask", "version", "limits", "imginfo", "abi", "features", "kill", "killall", "pgrep", "pidof", "nproc", "lscpu", "cpuinfo", "schedstat", "scheddebug", "vmstat", "top", "pstree", "uyield", "ubusy", "slowcat", "burst", "loop", "devio", "tty", "ttystat", "stty", "ttyread", "clear",
};

bool starts_with(const char* text, const char* prefix) {
    if (!text || !prefix) return false;
    for (uint64_t i = 0; prefix[i] != 0; ++i) {
        if (text[i] != prefix[i]) return false;
    }
    return true;
}

void append_completion_suffix(char (&line)[kLineCapacity], uint64_t& length, uint64_t& cursor, const char* match, uint64_t token_start) {
    uint64_t token_len = cursor - token_start;
    for (uint64_t i = token_len; match[i] != 0 && length + 1 < kLineCapacity; ++i) {
        for (uint64_t j = length + 1; j > cursor; --j) line[j] = line[j - 1];
        line[cursor++] = match[i];
        ++length;
        line[length] = 0;
    }
}

void list_completion(const char* prefix, const char* value) {
    char out[128];
    uint64_t out_cursor = 0;
    append_text(out, sizeof(out), out_cursor, "[complete] ");
    append_text(out, sizeof(out), out_cursor, prefix);
    append_text(out, sizeof(out), out_cursor, value);
    shell_emit(out);
}

uint64_t complete_command_token(char (&line)[kLineCapacity], uint64_t& length, uint64_t& cursor) {
    char token[kLineCapacity];
    for (uint64_t i = 0; i < sizeof(token); ++i) token[i] = 0;
    for (uint64_t i = 0; i < cursor && i + 1 < sizeof(token); ++i) token[i] = line[i];

    uint64_t matches = 0;
    const char* match = nullptr;
    for (uint64_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i) {
        if (!starts_with(kCommands[i].name, token)) continue;
        match = kCommands[i].name;
        ++matches;
        if (matches > 1) list_completion("cmd ", kCommands[i].name);
    }
    for (uint64_t i = 0; i < sizeof(kExternalCommands) / sizeof(kExternalCommands[0]); ++i) {
        if (!starts_with(kExternalCommands[i], token)) continue;
        match = kExternalCommands[i];
        ++matches;
        if (matches > 1) list_completion("cmd ", kExternalCommands[i]);
    }
    if (matches == 0) return 0;
    if (matches == 1 && match) {
        append_completion_suffix(line, length, cursor, match, 0);
        if (length + 1 < kLineCapacity) {
            line[cursor++] = ' ';
            ++length;
            line[length] = 0;
        }
    } else {
        write_terminal("\n");
    }
    return matches;
}

uint64_t complete_path_token(char (&line)[kLineCapacity], uint64_t& length, uint64_t& cursor) {
    uint64_t token_start = cursor;
    while (token_start != 0 && line[token_start - 1] != ' ' && line[token_start - 1] != '\t') --token_start;
    char token[kLineCapacity];
    for (uint64_t i = 0; i < sizeof(token); ++i) token[i] = 0;
    for (uint64_t i = token_start; i < cursor && (i - token_start) + 1 < sizeof(token); ++i) token[i - token_start] = line[i];
    if (token[0] == 0) return 0;

    auto count = syscall(hybrid::SyscallNumber::GetVfsNodeCount);
    if (count.error != hybrid::kSyscallErrorNone) return 0;
    uint64_t matches = 0;
    const char* match = nullptr;
    char match_copy[64];
    for (uint64_t i = 0; i < count.value && i < 128; ++i) {
        hybrid::VfsNodeInfo node;
        auto result = syscall(hybrid::SyscallNumber::GetVfsNodeInfo, i, reinterpret_cast<uint64_t>(&node));
        if (result.error != hybrid::kSyscallErrorNone || !starts_with(node.path, token)) continue;
        copy_text(match_copy, sizeof(match_copy), node.path);
        match = match_copy;
        ++matches;
        if (matches > 1) list_completion("path ", node.path);
    }
    if (matches == 0) return 0;
    if (matches == 1 && match) {
        append_completion_suffix(line, length, cursor, match, token_start);
    } else {
        write_terminal("\n");
    }
    return matches;
}

void complete_input(char (&line)[kLineCapacity], uint64_t& length, uint64_t& cursor) {
    uint64_t visual_cursor = cursor;
    bool command_token = true;
    for (uint64_t i = 0; i < cursor; ++i) {
        if (line[i] == ' ' || line[i] == '\t') {
            command_token = false;
            break;
        }
    }
    uint64_t matches = command_token ? complete_command_token(line, length, cursor) : complete_path_token(line, length, cursor);
    if (matches == 0) return;
    if (line[length] != 0) line[length] = 0;
    if (matches == 1) {
        uint64_t target = cursor;
        cursor = visual_cursor;
        redraw_input(line, length, cursor, target);
    } else {
        write_terminal(shell_prompt_text);
        write_terminal(line);
        uint64_t visual = length;
        while (visual > cursor) {
            write_terminal("\b");
            --visual;
        }
    }
}

bool spawn_external_command(
    const char* name,
    const char* argument,
    const char* stdout_path,
    const char* stdin_path,
    const char* stderr_path,
    bool stdout_append,
    bool stderr_append,
    uint32_t stdin_pipe,
    uint32_t stdout_pipe,
    uint64_t& out_pid,
    bool start_after_setup,
    const char* trace_prefix);

bool exec_external_command(
    const char* name,
    const char* argument,
    const char* stdout_path = nullptr,
    const char* stdin_path = nullptr,
    const char* stderr_path = nullptr,
    bool stdout_append = false,
    bool stderr_append = false,
    uint32_t stdin_pipe = 0,
    uint32_t stdout_pipe = 0) {
    uint64_t pid = 0;
    if (!spawn_external_command(name, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append, stdin_pipe, stdout_pipe, pid, false, "exec")) return false;

    set_terminal_foreground_group(pid, "fgpgid exec ");
    shell_line("exec ", "scheduled");
    auto started = syscall(hybrid::SyscallNumber::StartProcess, pid);
    if (started.error != hybrid::kSyscallErrorNone) {
        shell_hex("exec start error ", started.error);
        restore_shell_foreground_group();
        shell_last_status = 126;
        return true;
    }
    uint64_t waited_code = 0;
    uint64_t polls = 0;
    for (;;) {
        auto waited = syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&waited_code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        ++polls;
        poll_job_control_for_process_group(pid);
        syscall(hybrid::SyscallNumber::Yield);
    }
    restore_shell_foreground_group();
    shell_last_status = waited_code;
    shell_hex("exec exit ", waited_code);
    shell_hex("exec wait ", waited_code);
    shell_hex("exec wait polls ", polls);

    auto reaped = syscall(hybrid::SyscallNumber::ReapProcess, pid);
    if (reaped.error == hybrid::kSyscallErrorNone && reaped.value != 0) shell_hex("exec reap ", pid);
    else shell_hex("exec reap error ", reaped.error);
    return true;
}

bool run_background_command(
    const char* name,
    const char* argument,
    const char* stdout_path,
    const char* stdin_path,
    const char* stderr_path,
    bool stdout_append,
    bool stderr_append) {
    uint64_t pid = 0;
    if (!spawn_external_command(name, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append, 0, 0, pid, false, "bg")) return false;
    char command_line[64];
    uint64_t cursor = 0;
    append_text(command_line, sizeof(command_line), cursor, name);
    if (argument && argument[0] != 0) {
        append_char(command_line, sizeof(command_line), cursor, ' ');
        append_text(command_line, sizeof(command_line), cursor, argument);
    }
    remember_job(pid, command_line);
    if (ShellJob* job = find_job(pid)) shell_decimal("bg job ", job->job_id);
    shell_hex("bg pid ", pid);
    shell_line("bg ", "scheduled");
    auto started = syscall(hybrid::SyscallNumber::StartProcess, pid);
    if (started.error != hybrid::kSyscallErrorNone) {
        shell_hex("bg start error ", started.error);
        shell_last_status = 126;
        return true;
    }
    shell_last_status = 0;
    return true;
}

bool spawn_external_command(
    const char* name,
    const char* argument,
    const char* stdout_path,
    const char* stdin_path,
    const char* stderr_path,
    bool stdout_append,
    bool stderr_append,
    uint32_t stdin_pipe,
    uint32_t stdout_pipe,
    uint64_t& out_pid,
    bool start_after_setup,
    const char* trace_prefix) {
    char command_line[128];
    uint64_t cursor = 0;
    if (name[0] == '/') {
        append_text(command_line, sizeof(command_line), cursor, name);
    } else {
        char resolved[64];
        if (!resolve_external_path(name, resolved)) return false;
        append_text(command_line, sizeof(command_line), cursor, resolved);
    }
    if (argument && argument[0] != 0) {
        append_char(command_line, sizeof(command_line), cursor, ' ');
        append_text(command_line, sizeof(command_line), cursor, argument);
    }

    uint64_t pid = 0;
    auto spawned = syscall(hybrid::SyscallNumber::Spawn,
                           reinterpret_cast<uint64_t>(command_line),
                           strlen(command_line) + 1,
                           reinterpret_cast<uint64_t>(&pid),
                           hybrid::SpawnFlagStartSuspended);
    if (pid == 0) pid = spawned.value;
    if (pid == 0) return false;
    last_spawn_pid = pid;
    out_pid = pid;
    if (trace_prefix && trace_prefix[0] != 0) {
        char label[32];
        uint64_t label_cursor = 0;
        append_text(label, sizeof(label), label_cursor, trace_prefix);
        append_text(label, sizeof(label), label_cursor, " pid ");
        shell_hex(label, pid);
    }

    if (stdin_pipe != 0) {
        auto attached = syscall(hybrid::SyscallNumber::AttachPipeFd, pid, hybrid::kStdinFd, stdin_pipe, static_cast<uint64_t>(hybrid::PipeEndpoint::Read));
        if (attached.error != hybrid::kSyscallErrorNone) {
            shell_hex("pipe stdin error ", attached.error);
            shell_last_status = 126;
            return true;
        }
        shell_hex("pipe stdin ", stdin_pipe);
    } else if (stdin_path && stdin_path[0] != 0) {
        auto redirected = syscall(hybrid::SyscallNumber::RedirectProcessFd, pid, hybrid::kStdinFd, reinterpret_cast<uint64_t>(stdin_path), strlen(stdin_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) {
            shell_hex("redirect stdin error ", redirected.error);
            shell_last_status = 126;
            return true;
        }
        shell_path("redirect stdin ", stdin_path);
    }

    if (stdout_pipe != 0) {
        auto attached = syscall(hybrid::SyscallNumber::AttachPipeFd, pid, hybrid::kStdoutFd, stdout_pipe, static_cast<uint64_t>(hybrid::PipeEndpoint::Write));
        if (attached.error != hybrid::kSyscallErrorNone) {
            shell_hex("pipe stdout error ", attached.error);
            shell_last_status = 126;
            return true;
        }
        shell_hex("pipe stdout ", stdout_pipe);
    } else if (stdout_path && stdout_path[0] != 0) {
        if (stdout_append) {
            auto exists = syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(stdout_path), strlen(stdout_path) + 1);
            if (exists.error != hybrid::kSyscallErrorNone) {
                auto created = syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(stdout_path), strlen(stdout_path) + 1);
                if (created.error != hybrid::kSyscallErrorNone) {
                    shell_hex("redirect append create error ", created.error);
                    shell_last_status = 126;
                    return true;
                }
            }
        } else {
            syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(stdout_path), strlen(stdout_path) + 1);
            auto created = syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(stdout_path), strlen(stdout_path) + 1);
            if (created.error != hybrid::kSyscallErrorNone) {
                shell_hex("redirect create error ", created.error);
                shell_last_status = 126;
                return true;
            }
        }
        auto redirect_call = stdout_append ? hybrid::SyscallNumber::RedirectProcessFdAppend : hybrid::SyscallNumber::RedirectProcessFd;
        auto redirected = syscall(redirect_call, pid, hybrid::kStdoutFd, reinterpret_cast<uint64_t>(stdout_path), strlen(stdout_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) {
            shell_hex("redirect fd error ", redirected.error);
            shell_last_status = 126;
            return true;
        }
        shell_path(stdout_append ? "redirect append stdout " : "redirect stdout ", stdout_path);
    }

    if (stderr_path && stderr_path[0] != 0) {
        if (stderr_append) {
            auto exists = syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(stderr_path), strlen(stderr_path) + 1);
            if (exists.error != hybrid::kSyscallErrorNone) {
                auto created = syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(stderr_path), strlen(stderr_path) + 1);
                if (created.error != hybrid::kSyscallErrorNone) {
                    shell_hex("redirect append stderr create error ", created.error);
                    shell_last_status = 126;
                    return true;
                }
            }
        } else {
            syscall(hybrid::SyscallNumber::DeleteFile, reinterpret_cast<uint64_t>(stderr_path), strlen(stderr_path) + 1);
            auto created = syscall(hybrid::SyscallNumber::CreateFile, reinterpret_cast<uint64_t>(stderr_path), strlen(stderr_path) + 1);
            if (created.error != hybrid::kSyscallErrorNone) {
                shell_hex("redirect stderr create error ", created.error);
                shell_last_status = 126;
                return true;
            }
        }
        auto redirect_call = stderr_append ? hybrid::SyscallNumber::RedirectProcessFdAppend : hybrid::SyscallNumber::RedirectProcessFd;
        auto redirected = syscall(redirect_call, pid, hybrid::kStderrFd, reinterpret_cast<uint64_t>(stderr_path), strlen(stderr_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) {
            shell_hex("redirect stderr fd error ", redirected.error);
            shell_last_status = 126;
            return true;
        }
        shell_path(stderr_append ? "redirect append stderr " : "redirect stderr ", stderr_path);
    }
    if (start_after_setup) {
        auto started = syscall(hybrid::SyscallNumber::StartProcess, pid);
        if (started.error != hybrid::kSyscallErrorNone) {
            shell_hex("start error ", started.error);
            shell_last_status = 126;
            return true;
        }
    }
    return true;
}

bool parse_command_fragment(
    char* fragment,
    char*& command,
    char*& argument,
    char*& stdout_path,
    char*& stdin_path,
    char*& stderr_path,
    bool& stdout_append,
    bool& stderr_append) {
    while (*fragment == ' ' || *fragment == '\t') ++fragment;
    if (*fragment == 0) return false;
    command = fragment;
    while (*fragment != 0 && *fragment != ' ' && *fragment != '\t') ++fragment;
    if (*fragment != 0) {
        *fragment++ = 0;
        while (*fragment == ' ' || *fragment == '\t') ++fragment;
    }
    argument = fragment;
    stdout_path = nullptr;
    stdin_path = nullptr;
    stderr_path = nullptr;
    stdout_append = false;
    stderr_append = false;
    for (char* scan = argument; *scan != 0; ++scan) {
        bool fd2 = false;
        if (*scan == '2' && scan[1] == '>') {
            fd2 = true;
        } else if (*scan != '>' && *scan != '<') {
            continue;
        }
        char op = fd2 ? '2' : *scan;
        char* marker = scan;
        if (fd2) {
            *scan++ = 0;
            *scan++ = 0;
        } else {
            *scan++ = 0;
        }
        bool append = false;
        if ((op == '>' || op == '2') && *scan == '>') {
            append = true;
            ++scan;
        }
        while (marker > argument && (marker[-1] == ' ' || marker[-1] == '\t')) *--marker = 0;
        while (*scan == ' ' || *scan == '\t') ++scan;
        if (*scan != 0) {
            if (op == '>') {
                stdout_path = scan;
                stdout_append = append;
            } else if (op == '2') {
                stderr_path = scan;
                stderr_append = append;
            } else {
                stdin_path = scan;
            }
            while (*scan != 0 && *scan != ' ' && *scan != '\t') ++scan;
            *scan = 0;
        }
    }
    char* end = argument;
    while (*end != 0) ++end;
    while (end > argument && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return true;
}

bool run_pipeline_segment(char* line, bool background = false) {
    constexpr uint64_t kMaxPipelineStages = 4;
    char command_line[64];
    copy_text(command_line, sizeof(command_line), line);
    char* stages[kMaxPipelineStages];
    for (uint64_t i = 0; i < kMaxPipelineStages; ++i) stages[i] = nullptr;
    uint64_t stage_count = 0;
    char* cursor = line;
    while (stage_count < kMaxPipelineStages) {
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        if (*cursor == 0) break;
        stages[stage_count++] = cursor;
        while (*cursor != 0 && *cursor != '|') ++cursor;
        if (*cursor == '|') *cursor++ = 0;
    }
    if (stage_count < 2) return false;
    shell_hex("pipe stages ", stage_count);

    uint32_t pipes[kMaxPipelineStages - 1];
    uint64_t pids[kMaxPipelineStages];
    for (uint64_t i = 0; i < kMaxPipelineStages - 1; ++i) pipes[i] = 0;
    for (uint64_t i = 0; i < kMaxPipelineStages; ++i) pids[i] = 0;
    uint64_t process_group_id = 0;

    for (uint64_t i = 0; i + 1 < stage_count; ++i) {
        auto pipe = syscall(hybrid::SyscallNumber::CreatePipe);
        if (pipe.error != hybrid::kSyscallErrorNone || pipe.value == 0) {
            shell_hex("pipe create error ", pipe.error);
            for (uint64_t j = 0; j < i; ++j) if (pipes[j] != 0) syscall(hybrid::SyscallNumber::ClosePipe, pipes[j]);
            shell_last_status = 126;
            return true;
        }
        pipes[i] = static_cast<uint32_t>(pipe.value);
        shell_hex("pipe create ", pipes[i]);
    }

    for (uint64_t i = 0; i < stage_count; ++i) {
        char* command = nullptr;
        char* argument = nullptr;
        char* stdout_path = nullptr;
        char* stdin_path = nullptr;
        char* stderr_path = nullptr;
        bool stdout_append = false;
        bool stderr_append = false;
        if (!parse_command_fragment(stages[i], command, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append)) {
            shell_hex("pipe parse error ", i);
            shell_last_status = 126;
            return true;
        }
        char expanded_command[64];
        char expanded_argument[128];
        char expanded_stdout[64];
        char expanded_stdin[64];
        char expanded_stderr[64];
        expand_shell_variables(command, expanded_command, sizeof(expanded_command));
        expand_shell_variables(argument, expanded_argument, sizeof(expanded_argument));
        if (stdout_path) expand_shell_variables(stdout_path, expanded_stdout, sizeof(expanded_stdout));
        if (stdin_path) expand_shell_variables(stdin_path, expanded_stdin, sizeof(expanded_stdin));
        if (stderr_path) expand_shell_variables(stderr_path, expanded_stderr, sizeof(expanded_stderr));
        command = expanded_command;
        argument = expanded_argument;
        stdout_path = stdout_path ? expanded_stdout : nullptr;
        stdin_path = stdin_path ? expanded_stdin : nullptr;
        stderr_path = stderr_path ? expanded_stderr : nullptr;
        bool final_stage = i + 1 == stage_count;
        uint32_t input_pipe = stdin_path ? 0 : (i == 0 ? 0 : pipes[i - 1]);
        uint32_t output_pipe = final_stage ? 0 : pipes[i];
        const char* stage_output = stdout_path;
        shell_path("pipe stage ", command);
        if (!spawn_external_command(command, argument, final_stage ? stage_output : nullptr, stdin_path, stderr_path, final_stage && stdout_append, stderr_append, input_pipe, output_pipe, pids[i], false, "pipe pid")) {
            shell_path("pipe unknown ", command);
            for (uint64_t j = 0; j + 1 < stage_count; ++j) if (pipes[j] != 0) syscall(hybrid::SyscallNumber::ClosePipe, pipes[j]);
            shell_last_status = 127;
            return true;
        }
        if (process_group_id == 0) {
            process_group_id = pids[i];
            shell_hex("pipe pgid ", process_group_id);
        }
        auto grouped = syscall(hybrid::SyscallNumber::SetProcessGroup, pids[i], process_group_id);
        if (grouped.error != hybrid::kSyscallErrorNone) {
            shell_hex("pipe pgid error ", grouped.error);
            for (uint64_t j = 0; j + 1 < stage_count; ++j) if (pipes[j] != 0) syscall(hybrid::SyscallNumber::ClosePipe, pipes[j]);
            shell_last_status = 126;
            return true;
        }
        shell_hex("pipe group pid ", pids[i]);
    }
    for (uint64_t i = 0; i < stage_count; ++i) {
        shell_hex("pipe start pid ", pids[i]);
        auto started = syscall(hybrid::SyscallNumber::StartProcess, pids[i]);
        if (started.error != hybrid::kSyscallErrorNone) {
            shell_hex("pipe start error ", started.error);
            for (uint64_t j = 0; j + 1 < stage_count; ++j) if (pipes[j] != 0) syscall(hybrid::SyscallNumber::ClosePipe, pipes[j]);
            shell_last_status = 126;
            return true;
        }
    }
    remember_pipeline_job(process_group_id, pids, stage_count, command_line);
    if (ShellJob* job = find_job(process_group_id)) shell_decimal("pipe job ", job->job_id);
    shell_line("pipe ", "concurrent");
    if (background) {
        shell_hex("bg pipe pgid ", process_group_id);
        shell_line("bg pipe ", "scheduled");
        shell_last_status = 0;
        return true;
    }
    set_terminal_foreground_group(process_group_id, "fgpgid pipe ");
    uint64_t final_status = 0;
    uint64_t total_polls = 0;
    ShellJob* pipeline_job = find_job(process_group_id);
    if (pipeline_job) {
        wait_job_processes(*pipeline_job, true, final_status, total_polls);
        shell_hex("pipe wait polls ", total_polls);
    }
    restore_shell_foreground_group();
    if (pipeline_job) reap_job_processes(*pipeline_job);
    for (uint64_t i = 0; i + 1 < stage_count; ++i) if (pipes[i] != 0) syscall(hybrid::SyscallNumber::ClosePipe, pipes[i]);
    shell_last_status = final_status;
    shell_line("pipe ", "complete");
    return true;
}

bool strip_background_suffix(char* line) {
    char* end = line;
    while (*end != 0) ++end;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) --end;
    if (end == line || end[-1] != '&') return false;
    if (end >= line + 2 && end[-2] == '&') return false;
    --end;
    *end = 0;
    while (end > line && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return true;
}

void run_command(const char* name, const char* argument = "", const char* stdout_path = nullptr, const char* stdin_path = nullptr, const char* stderr_path = nullptr, bool stdout_append = false, bool stderr_append = false) {
    if (shell_trace_commands) shell_prompt(name);
    for (uint64_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i) {
        if (streq(name, kCommands[i].name)) {
            kCommands[i].handler(argument);
            shell_last_status = 0;
            return;
        }
    }
    if (exec_external_command(name, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append)) return;
    shell_path("unknown command ", name);
    shell_last_status = 127;
}

void run_shell_line(char* line);
bool argument_present(const char* expected) {
    auto count = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::ArgumentInfo argument;
        if (syscall(hybrid::SyscallNumber::GetArgument, i, reinterpret_cast<uint64_t>(&argument)).error == hybrid::kSyscallErrorNone &&
            streq(argument.value, expected)) {
            return true;
        }
    }
    return false;
}

bool boot_script_requested() {
    return argument_present("--boot");
}

bool recovery_requested() {
    return argument_present("--recovery");
}

void enter_recovery_shell_mode() {
    shell_prompt_text = "recovery# ";
}

void trim_recovery_segment(char*& text) {
    while (*text == ' ' || *text == '\t') ++text;
    char* end = text;
    while (*end != 0) ++end;
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
}

void split_recovery_command(char* line, char*& command, char*& argument) {
    command = line;
    trim_recovery_segment(command);
    argument = command;
    while (*argument != 0 && *argument != ' ' && *argument != '\t') ++argument;
    if (*argument != 0) *argument++ = 0;
    trim_recovery_segment(argument);
}

void recovery_help() {
    shell_emit("recovery commands:");
    shell_emit("  status      show boot, process, memory, cpu, and framebuffer state");
    shell_emit("  check       verify core rescue paths and boot media visibility");
    shell_emit("  logs        print kernel log");
    shell_emit("  mounts      show mounted filesystems and capacity");
    shell_emit("  files       list important rescue directories");
    shell_emit("  processes   show process and thread table");
    shell_emit("  hardware    show detected CPUs, devices, and framebuffer");
    shell_emit("  shell       leave rescue target and start the normal shell");
    shell_emit("  clear exit  clear the screen or halt this userspace session");
}

void recovery_status() {
    run_command("sysinfo");
    run_command("pid");
    run_command("ids");
    run_command("mem");
    run_command("cpus");
    run_command("fb");
}

void recovery_check() {
    shell_emit("checking rescue filesystem paths");
    run_command("stat", "/");
    run_command("stat", "/bin");
    run_command("stat", "/dev/console");
    run_command("stat", "/dev/tty");
    run_command("stat", "/proc/mounts");
    run_command("stat", "/proc/meminfo");
    run_command("stat", "/mnt/boot/kernel.elf");
    run_command("stat", "/mnt/boot/user/init.elf");
}

void recovery_logs() {
    run_command("dmesg");
}

void recovery_mounts() {
    run_command("mount");
    run_command("df");
    run_command("cat", "/proc/mounts");
}

void recovery_files() {
    run_command("ls", "/");
    run_command("ls", "/bin");
    run_command("ls", "/dev");
    run_command("ls", "/proc");
    run_command("ls", "/mnt/boot");
}

void recovery_processes() {
    run_command("ps");
}

void recovery_hardware() {
    run_command("cpus");
    run_command("devices");
    run_command("fb");
}

bool recovery_passthrough_allowed(const char* command) {
    return streq(command, "cat") || streq(command, "cd") || streq(command, "df") ||
        streq(command, "dmesg") || streq(command, "kmsg") || streq(command, "loadavg") || streq(command, "scheddebug") || streq(command, "devices") || streq(command, "fb") ||
        streq(command, "ls") || streq(command, "mem") || streq(command, "mount") || streq(command, "buddyinfo") || streq(command, "heapinfo") || streq(command, "procvmstat") ||
        streq(command, "ps") || streq(command, "pwd") || streq(command, "stat") ||
        streq(command, "sysinfo") || streq(command, "ticks") || streq(command, "uname");
}

void run_recovery_line(char* line);

[[noreturn]] void recovery_shell_loop();
[[noreturn]] void interactive_shell_loop();

void run_boot_shell_script() {
    shell_trace_commands = true;
    shell_tag_output = true;
    shell_emit("[shell] boot shell starting");
    run_command("fgpgid");
    run_command("help");
    run_command("echo", "stdout fd online");
    run_command("pid");
    run_command("ids");
    run_command("ctx");
    run_command("argv");
    run_command("env");
    run_command("export", "EDITOR=hksh");
    run_command("export", "LONGVAR=alpha-beta-gamma-delta-epsilon-zeta-eta-theta-iota-kappa");
    run_command("env");
    char expansion_line[128];
    copy_text(expansion_line, sizeof(expansion_line), "echo env-$EDITOR ; echo long-$LONGVAR ; false ; echo status-$? ; echo path-${PATH}");
    run_shell_line(expansion_line);
    run_command("which", "grep");
    run_command("stat", "/bin/hello.elf");
    run_command("stat", "/etc/os-release");
    run_command("stat", "/etc/hostname");
    run_command("stat", "/tmp");
    run_command("stat", "/dev/null");
    run_command("stat", "/dev/zero");
    run_command("stat", "/dev/tty");
    run_command("stat", "/dev/console");
    run_command("stat", "/proc/meminfo");
    run_command("stat", "/proc/iomem");
    run_command("stat", "/proc/rtc");
    run_command("stat", "/proc/stat");
    run_command("stat", "/proc/loadavg");
    run_command("stat", "/proc/sched_debug");
    run_command("stat", "/proc/modules");
    run_command("stat", "/proc/kmsg");
    run_command("stat", "/proc/block/bootdisk");
    run_command("stat", "/proc/diskstats");
    run_command("stat", "/proc/partitions");
    run_command("stat", "/proc/devices");
    run_command("stat", "/proc/driver/summary");
    run_command("stat", "/proc/driver/devices");
    run_command("stat", "/proc/pci/summary");
    run_command("stat", "/proc/pci/devices");
    run_command("stat", "/proc/irq/summary");
    run_command("stat", "/proc/interrupts");
    run_command("stat", "/proc/tty/summary");
    run_command("stat", "/proc/cpuinfo");
    run_command("stat", "/proc/cpu/summary");
    run_command("stat", "/proc/cpu/topology");
    run_command("stat", "/proc/heapinfo");
    run_command("stat", "/proc/vmstat");
    run_command("stat", "/proc/net/summary");
    run_command("stat", "/proc/net/dev");
    run_command("stat", "/proc/net/route");
    run_command("stat", "/proc/bootinfo");
    run_command("stat", "/proc/buddyinfo");
    run_command("stat", "/proc/mounts");
    run_command("stat", "/proc/filesystems");
    run_command("stat", "/proc/fs/vfs");
    run_command("stat", "/proc/cmdline");
    run_command("stat", "/proc/sys/kernel/hostname");
    run_command("stat", "/proc/sys/kernel/ostype");
    run_command("stat", "/proc/sys/kernel/osrelease");
    run_command("stat", "/proc/sys/kernel/pid_max");
    run_command("stat", "/proc/sys/kernel/threads-max");
    run_command("stat", "/proc/sys/kernel/version");
    run_command("stat", "/proc/self/status");
    run_command("stat", "/proc/self/stat");
    run_command("stat", "/proc/self/maps");
    run_command("stat", "/proc/self/cmdline");
    run_command("stat", "/proc/self/comm");
    run_command("stat", "/proc/self/environ");
    run_command("stat", "/proc/self/cwd");
    run_command("stat", "/proc/self/exe");
    run_command("stat", "/proc/self/root");
    run_command("stat", "/proc/self/fd");
    run_command("stat", "/proc/self/fdinfo");
    run_command("stat", "/proc/self/limits");
    run_command("stat", "/proc/self/io");
    run_command("stat", "/proc/self/task");
    run_command("stat", "/proc/1");
    run_command("stat", "/proc/1/status");
    run_command("stat", "/proc/1/stat");
    run_command("stat", "/proc/1/maps");
    run_command("stat", "/proc/1/cmdline");
    run_command("stat", "/proc/1/comm");
    run_command("stat", "/proc/1/environ");
    run_command("stat", "/proc/1/cwd");
    run_command("stat", "/proc/1/exe");
    run_command("stat", "/proc/1/root");
    run_command("stat", "/proc/1/fd");
    run_command("stat", "/proc/1/fdinfo");
    run_command("stat", "/proc/1/fdinfo/1");
    run_command("stat", "/proc/1/limits");
    run_command("stat", "/proc/1/task");
    run_command("stat", "/proc/1/task/1");
    run_command("stat", "/proc/1/task/1/status");
    run_command("stat", "/proc/1/task/1/stat");
    run_command("stat", "/disk/bootsector.bin");
    run_command("stat", "/mnt/boot/kernel.elf");
    run_command("stat", "/mnt/boot/bin/hello.elf");
    run_command("counts");
    run_command("fds");
    run_command("pwd");
    run_command("ls");
    run_command("ls", "/bin");
    run_command("ls", "/dev");
    run_command("cd", "/user");
    run_command("pwd");
    run_command("stat", "../user/./init.elf");
    run_command("cat", "init.elf");
    run_command("cd", "/");
    run_command("spawn", "/bin/hello.elf alpha beta");
    run_command("jobs");
    run_command("usched");
    run_command("nextuser");
    run_command("run");
    run_command("wait");
    run_command("reap");
    run_command("jobs");
    run_command("uyielddemo");
    run_command("upreemptdemo");
    run_command("usched");
    run_command("spawn", "/bin/sleep.elf 5");
    run_command("jobs");
    run_command("kill", "-9");
    run_command("jobs");
    run_command("wait");
    run_command("reap");
    run_command("spawn", "/bin/sleep.elf 5");
    run_command("kill", "TERM");
    run_command("jobs");
    run_command("wait");
    run_command("reap");
    run_command("jobs");
    char background_sleep[32];
    copy_text(background_sleep, sizeof(background_sleep), "loop &");
    run_shell_line(background_sleep);
    run_command("jobs");
    run_command("stop", "%+");
    run_command("jobs");
    run_command("bg", "%+");
    run_command("jobs");
    run_command("kill", "TERM %+");
    run_command("jobs");
    run_command("wait", "%+");
    run_command("reap", "%+");
    run_command("jobs");
    char foreground_sleep[32];
    copy_text(foreground_sleep, sizeof(foreground_sleep), "sleep 5 &");
    run_shell_line(foreground_sleep);
    run_command("jobs");
    run_command("fg", "%+");
    run_command("jobs");
    char wait_any_loop[32];
    copy_text(wait_any_loop, sizeof(wait_any_loop), "loop &");
    run_shell_line(wait_any_loop);
    char wait_any_sleep[32];
    copy_text(wait_any_sleep, sizeof(wait_any_sleep), "sleep 5 &");
    run_shell_line(wait_any_sleep);
    run_command("wait", "-n");
    run_command("reap", "%+");
    run_command("kill", "TERM %+");
    run_command("wait", "%+");
    run_command("reap", "%+");
    run_command("jobs");
    run_command("hello", "gamma delta");
    run_command("args", "one two");
    run_command("args", "\"two words\" 'single quoted' escaped\\ space");
    run_command("/bin/cat.elf", "/bin/args.elf");
    run_command("/bin/stat.elf", "/tmp");
    run_command("/bin/statfs.elf", "/");
    run_command("/bin/statfs.elf", "/mnt/boot/bin/hello.elf");
    run_command("/bin/filesystems.elf");
    run_command("/bin/vfsstat.elf");
    run_command("whoami");
    run_command("basename", "/user/init.elf");
    run_command("dirname", "/user/init.elf");
    run_command("head", "/etc/os-release");
    run_command("head", "-n 2 /etc/os-release");
    run_command("tail", "/etc/os-release");
    run_command("tail", "-n 2 /etc/os-release");
    char test_chain[128];
    copy_text(test_chain, sizeof(test_chain), "test -d /tmp && echo test-dir-ok ; test -f /bin/hello.elf && echo test-file-ok ; test -e /missing || echo test-missing-ok");
    run_shell_line(test_chain);
    run_command("/bin/ls.elf", "/bin");
    run_command("/bin/ls.elf", "/etc");
    run_command("/bin/ls.elf", "/mnt/boot");
    run_command("/bin/ls.elf", "/disk");
    run_command("/bin/ls.elf", "/dev");
    run_command("/bin/ls.elf", "/proc");
    run_command("/bin/ls.elf", "/proc/block");
    run_command("/bin/ls.elf", "/proc/driver");
    run_command("/bin/ls.elf", "/proc/pci");
    run_command("/bin/ls.elf", "/proc/irq");
    run_command("/bin/ls.elf", "/proc/tty");
    run_command("/bin/ls.elf", "/proc/cpu");
    run_command("/bin/ls.elf", "/proc/net");
    run_command("/bin/ls.elf", "/proc/self");
    run_command("/bin/ls.elf", "/proc/self/task");
    run_command("/bin/ls.elf", "/proc/fs");
    run_command("/bin/ls.elf", "/proc/sys");
    run_command("/bin/ls.elf", "/proc/sys/kernel");
    run_command("/bin/ls.elf", "/proc/1");
    run_command("/bin/ls.elf", "/proc/1/task");
    run_command("/bin/ls.elf", "/proc/1/task/1");
    run_command("/bin/ls.elf", "/mnt/boot/bin");
    run_command("/bin/ls.elf", "/mnt/boot/user");
    run_command("/bin/cat.elf", "/etc/os-release");
    run_command("/bin/cat.elf", "/etc/hostname");
    run_command("/bin/cat.elf", "/etc/hostname /proc/sys/kernel/hostname");
    run_command("/bin/cat.elf", "/proc/version");
    run_command("/bin/cat.elf", "/proc/meminfo");
    run_command("/bin/cat.elf", "/proc/iomem");
    run_command("/bin/cat.elf", "/proc/rtc");
    run_command("/bin/cat.elf", "/proc/uptime");
    run_command("/bin/cat.elf", "/proc/loadavg");
    run_command("/bin/cat.elf", "/proc/sched_debug");
    run_command("/bin/cat.elf", "/proc/stat");
    run_command("/bin/cat.elf", "/proc/modules");
    run_command("/bin/cat.elf", "/proc/kmsg");
    run_command("/bin/cat.elf", "/proc/block/bootdisk");
    run_command("/bin/cat.elf", "/proc/diskstats");
    run_command("/bin/cat.elf", "/proc/partitions");
    run_command("/bin/cat.elf", "/proc/devices");
    run_command("/bin/cat.elf", "/proc/driver/summary");
    run_command("/bin/cat.elf", "/proc/driver/devices");
    run_command("/bin/cat.elf", "/proc/pci/summary");
    run_command("/bin/cat.elf", "/proc/pci/devices");
    run_command("/bin/cat.elf", "/proc/irq/summary");
    run_command("/bin/cat.elf", "/proc/interrupts");
    run_command("/bin/cat.elf", "/proc/tty/summary");
    run_command("/bin/cat.elf", "/proc/cpuinfo");
    run_command("/bin/cat.elf", "/proc/cpu/summary");
    run_command("/bin/cat.elf", "/proc/cpu/topology");
    run_command("/bin/cat.elf", "/proc/heapinfo");
    run_command("/bin/cat.elf", "/proc/vmstat");
    run_command("/bin/cat.elf", "/proc/net/summary");
    run_command("/bin/cat.elf", "/proc/net/dev");
    run_command("/bin/cat.elf", "/proc/net/route");
    run_command("/bin/cat.elf", "/proc/bootinfo");
    run_command("/bin/cat.elf", "/proc/features");
    run_command("/bin/cat.elf", "/proc/abi");
    run_command("/bin/cat.elf", "/proc/buddyinfo");
    run_command("/bin/cat.elf", "/proc/processes");
    run_command("/bin/cat.elf", "/proc/mounts");
    run_command("/bin/cat.elf", "/proc/filesystems");
    run_command("/bin/cat.elf", "/proc/fs/vfs");
    run_command("/bin/cat.elf", "/proc/cmdline");
    run_command("/bin/cat.elf", "/proc/sys/kernel/hostname");
    run_command("/bin/cat.elf", "/proc/sys/kernel/ostype");
    run_command("/bin/cat.elf", "/proc/sys/kernel/osrelease");
    run_command("/bin/cat.elf", "/proc/sys/kernel/pid_max");
    run_command("/bin/cat.elf", "/proc/sys/kernel/threads-max");
    run_command("/bin/cat.elf", "/proc/sys/kernel/version");
    run_command("/bin/cat.elf", "/proc/self/status");
    run_command("/bin/cat.elf", "/proc/self/stat");
    run_command("/bin/cat.elf", "/proc/self/maps");
    run_command("/bin/cat.elf", "/proc/self/cmdline");
    run_command("/bin/cat.elf", "/proc/self/comm");
    run_command("/bin/cat.elf", "/proc/self/environ");
    run_command("/bin/cat.elf", "/proc/self/cwd");
    run_command("/bin/cat.elf", "/proc/self/exe");
    run_command("/bin/cat.elf", "/proc/self/root");
    run_command("/bin/cat.elf", "/proc/self/fd");
    run_command("/bin/cat.elf", "/proc/self/fdinfo");
    run_command("/bin/cat.elf", "/proc/self/limits");
    run_command("/bin/cat.elf", "/proc/self/io");
    run_command("/bin/cat.elf", "/proc/1/status");
    run_command("/bin/cat.elf", "/proc/1/stat");
    run_command("/bin/cat.elf", "/proc/1/maps");
    run_command("/bin/cat.elf", "/proc/1/cmdline");
    run_command("/bin/cat.elf", "/proc/1/comm");
    run_command("/bin/cat.elf", "/proc/1/environ");
    run_command("/bin/cat.elf", "/proc/1/cwd");
    run_command("/bin/cat.elf", "/proc/1/exe");
    run_command("/bin/cat.elf", "/proc/1/root");
    run_command("/bin/cat.elf", "/proc/1/fd");
    run_command("/bin/cat.elf", "/proc/1/fdinfo");
    run_command("/bin/cat.elf", "/proc/1/fdinfo/1");
    run_command("/bin/cat.elf", "/proc/1/limits");
    char readlink_fd_line[80];
    copy_text(readlink_fd_line, sizeof(readlink_fd_line), "readlink /proc/self/fd/1 > /tmp/readlink.txt");
    run_shell_line(readlink_fd_line);
    run_command("/bin/cat.elf", "/tmp/readlink.txt");
    run_command("/bin/realpath.elf", "../user/./init.elf");
    char realpath_fd_line[80];
    copy_text(realpath_fd_line, sizeof(realpath_fd_line), "realpath /proc/self/fd/1 > /tmp/realpath.txt");
    run_shell_line(realpath_fd_line);
    run_command("/bin/cat.elf", "/tmp/realpath.txt");
    run_command("/bin/cat.elf", "/disk/bootsector.bin");
    run_command("/bin/hexdump.elf", "/disk/bootsector.bin");
    run_command("/bin/hexdump.elf", "/etc/os-release");
    run_command("/bin/file.elf", "/bin/hello.elf /etc/os-release /proc/version /dev/tty /bin");
    run_command("/bin/lsattr.elf", "/etc/os-release /proc/version /dev/tty /bin /mnt/boot/kernel.elf");
    run_command("/bin/namei.elf", "/mnt/boot/bin/hello.elf");
    run_command("/bin/tree.elf", "/mnt/boot");
    run_command("/bin/readelf.elf", "/bin/readelf.elf");
    run_command("/bin/readelf.elf", "/mnt/boot/kernel.elf");
    run_command("/bin/sha256sum.elf", "/etc/hostname");
    run_command("/bin/sha256sum.elf", "/etc/os-release");
    run_command("/bin/sha224sum.elf", "/etc/hostname");
    run_command("/bin/sha512sum.elf", "/etc/hostname");
    run_command("/bin/sha384sum.elf", "/etc/hostname");
    run_command("/bin/sha1sum.elf", "/etc/hostname");
    run_command("/bin/md5sum.elf", "/etc/hostname");
    run_command("/bin/cksum.elf", "/etc/hostname");
    run_command("/bin/fold.elf", "-w 12 /proc/version");
    run_command("/bin/printf.elf", "'%s:%d:%x\\n' IanOS 42 255");
    run_command("/bin/dd.elf", "if=/etc/hostname of=/tmp/dd-host bs=6 count=1");
    run_command("/bin/cat.elf", "/tmp/dd-host");
    run_command("/bin/wc.elf", "/tmp/dd-host");
    run_command("/bin/wc.elf", "/etc/hostname /proc/sys/kernel/hostname");
    run_command("/bin/cmp.elf", "/etc/hostname /proc/sys/kernel/hostname");
    run_command("/bin/cmp.elf", "/etc/hostname /etc/os-release");
    run_command("/bin/strings.elf", "/etc/os-release");
    run_command("/bin/strings.elf", "/proc/version");
    run_command("/bin/nl.elf", "/etc/os-release");
    run_command("/bin/tr.elf", "IOS ios /etc/os-release");
    run_command("/bin/sed.elf", "s/IanOS/IanOS-dev/ /etc/os-release");
    run_command("/bin/cut.elf", "-d = -f 2 /etc/os-release");
    run_command("/bin/paste.elf", "/etc/hostname /proc/sys/kernel/hostname");
    run_command("/bin/rev.elf", "/etc/hostname");
    run_command("/bin/tac.elf", "/etc/os-release");
    run_command("/bin/seq.elf", "3");
    run_command("/bin/expr.elf", "7 + 5");
    char xargs_line[96];
    copy_text(xargs_line, sizeof(xargs_line), "/bin/printf.elf 'alpha beta\\n' | /bin/xargs.elf /bin/echo.elf prefix");
    run_shell_line(xargs_line);
    run_command("/bin/yes.elf", "IanOS count=3");
    run_command("/bin/od.elf", "-t x1 /etc/hostname");
    run_command("/bin/base64.elf", "/etc/hostname");
    run_command("touch", "/tmp/host64");
    run_command("append", "/tmp/host64 aWFub3MK");
    run_command("/bin/base64.elf", "-d /tmp/host64");
    run_command("rm", "/tmp/host64");
    run_command("/bin/which.elf", "grep");
    run_command("/bin/cat.elf", "/mnt/boot/kernel.elf");
    run_command("/bin/cat.elf", "/mnt/boot/bin/hello.elf");
    run_command("uname");
    run_command("hostname");
    run_command("free");
    run_command("/bin/meminfo.elf");
    run_command("uptime");
    run_command("date");
    run_command("rtc");
    run_command("/bin/cal.elf", "7 2026");
    run_command("dmesg");
    run_command("/bin/kmsg.elf");
    run_command("/bin/loadavg.elf");
    run_command("/bin/scheddebug.elf");
    run_command("/bin/ps.elf");
    run_command("/bin/processes.elf");
    run_command("/bin/pwd.elf");
    run_command("/bin/env.elf");
    run_command("/bin/printenv.elf", "PATH");
    run_command("unset", "EDITOR");
    run_command("env");
    run_command("sysinfo");
    run_command("fastfetch");
    run_command("/bin/sysctl.elf", "-a");
    run_command("/bin/sysctl.elf", "kernel.osrelease");
    run_command("/bin/sysctl.elf", "kernel.pid_max");
    run_command("/bin/sysctl.elf", "kernel.threads-max");
    run_command("/bin/id.elf");
    run_command("/bin/ids.elf");
    run_command("/bin/groups.elf");
    run_command("/bin/ctx.elf");
    run_command("/bin/devio.elf");
    run_command("/bin/stty.elf");
    run_command("/bin/stty.elf", "canonical");
    run_command("/bin/stty.elf");
    run_command("/bin/stty.elf", "raw");
    run_command("/bin/stty.elf");
    run_command("/bin/ttyread.elf");
    run_command("/bin/tty.elf");
    run_command("/bin/ttystat.elf");
    run_command("/bin/clear.elf");
    run_command("/bin/blk.elf");
    run_command("/bin/mount.elf");
    run_command("/bin/df.elf");
    run_command("/bin/du.elf", "/mnt/boot");
    run_command("/bin/lsblk.elf");
    run_command("/bin/findmnt.elf");
    run_command("/bin/findmnt.elf", "/mnt/boot");
    run_command("stat", "/proc/mountinfo");
    run_command("stat", "/proc/self/mountinfo");
    run_command("/bin/cat.elf", "/proc/mountinfo");
    run_command("/bin/cat.elf", "/proc/self/mountinfo");
    run_command("/bin/mountinfo.elf");
    run_command("/bin/mountinfo.elf", "all");
    run_command("/bin/iostat.elf");
    run_command("/bin/diskstats.elf");
    run_command("/bin/partitions.elf");
    run_command("/bin/lsmem.elf");
    run_command("/bin/iomem.elf");
    run_command("/bin/bootinfo.elf");
    run_command("/bin/fbset.elf");
    run_command("/bin/lspci.elf");
    run_command("/bin/lsdev.elf");
    run_command("/bin/devices.elf");
    run_command("/bin/irqstat.elf");
    run_command("/bin/interrupts.elf");
    run_command("/bin/mmstat.elf");
    run_command("/bin/buddyinfo.elf");
    run_command("/bin/heapinfo.elf");
    run_command("/bin/procvmstat.elf");
    run_command("/bin/netstat.elf");
    run_command("/bin/route.elf");
    run_command("/bin/ip.elf");
    run_command("/bin/ip.elf", "link");
    run_command("/bin/ip.elf", "addr");
    run_command("/bin/ip.elf", "route");
    run_command("/bin/ifconfig.elf");
    run_command("/bin/ethtool.elf");
    run_command("/bin/lsdrv.elf");
    run_command("/bin/lsmod.elf");
    run_command("/bin/cmdline.elf");
    run_command("/bin/procstat.elf");
    run_command("/bin/pipeinfo.elf");
    run_command("/bin/pmap.elf");
    run_command("/bin/pmap.elf", "1");
    run_command("/bin/maps.elf");
    run_command("/bin/maps.elf", "1");
    run_command("/bin/pcmdline.elf");
    run_command("/bin/pcmdline.elf", "1");
    run_command("/bin/proccomm.elf");
    run_command("/bin/proccomm.elf", "1");
    run_command("/bin/procenv.elf");
    run_command("/bin/procenv.elf", "1");
    run_command("/bin/procwd.elf");
    run_command("/bin/procwd.elf", "1");
    run_command("/bin/procexe.elf");
    run_command("/bin/procexe.elf", "1");
    run_command("/bin/procroot.elf");
    run_command("/bin/procroot.elf", "1");
    run_command("/bin/procfdinfo.elf");
    run_command("/bin/procfdinfo.elf", "1");
    run_command("/bin/procfdinfo.elf", "1 1");
    run_command("/bin/proclimits.elf");
    run_command("/bin/proclimits.elf", "1");
    run_command("/bin/procio.elf");
    run_command("/bin/procio.elf", "1");
    run_command("/bin/proctask.elf");
    run_command("/bin/proctask.elf", "1");
    run_command("/bin/proctask.elf", "1 1");
    run_command("/bin/version.elf");
    run_command("/bin/limits.elf");
    run_command("/bin/imginfo.elf");
    run_command("/bin/abi.elf");
    run_command("/bin/features.elf");
    char external_kill_loop[32];
    copy_text(external_kill_loop, sizeof(external_kill_loop), "loop &");
    run_shell_line(external_kill_loop);
    uint64_t external_kill_pid = last_spawn_pid;
    char external_kill_pid_arg[24];
    uint64_t external_kill_pid_cursor = 0;
    append_hex(external_kill_pid_arg, sizeof(external_kill_pid_arg), external_kill_pid_cursor, external_kill_pid);
    char external_kill_command[64];
    uint64_t external_kill_cursor = 0;
    append_text(external_kill_command, sizeof(external_kill_command), external_kill_cursor, "/bin/kill.elf TERM ");
    append_text(external_kill_command, sizeof(external_kill_command), external_kill_cursor, external_kill_pid_arg);
    run_shell_line(external_kill_command);
    run_command("wait", external_kill_pid_arg);
    run_command("reap", external_kill_pid_arg);
    char external_killall_loop[32];
    copy_text(external_killall_loop, sizeof(external_killall_loop), "loop &");
    run_shell_line(external_killall_loop);
    uint64_t external_killall_pid = last_spawn_pid;
    char external_killall_pid_arg[24];
    uint64_t external_killall_pid_cursor = 0;
    append_hex(external_killall_pid_arg, sizeof(external_killall_pid_arg), external_killall_pid_cursor, external_killall_pid);
    run_command("/bin/pgrep.elf", "loop");
    run_command("/bin/pidof.elf", "loop");
    run_command("/bin/killall.elf", "TERM loop");
    run_command("wait", external_killall_pid_arg);
    run_command("reap", external_killall_pid_arg);
    run_command("/bin/nproc.elf", "");
    run_command("/bin/lscpu.elf", "");
    run_command("/bin/cpuinfo.elf", "");
    run_command("/bin/schedstat.elf", "");
    run_command("/bin/vmstat.elf", "");
    run_command("/bin/top.elf", "");
    run_command("/bin/pstree.elf", "");
    run_command("/bin/echo.elf", "external echo works");
    char sleep_helper[32];
    copy_text(sleep_helper, sizeof(sleep_helper), "loop &");
    run_shell_line(sleep_helper);
    uint64_t sleep_helper_pid = last_spawn_pid;
    run_command("sleep", "2");
    char kill_helper[32];
    uint64_t kill_cursor = 0;
    append_text(kill_helper, sizeof(kill_helper), kill_cursor, "TERM ");
    append_hex(kill_helper, sizeof(kill_helper), kill_cursor, sleep_helper_pid);
    run_command("kill", kill_helper);
    run_command("wait", kill_helper + 5);
    run_command("reap", kill_helper + 5);
    run_command("true");
    run_command("false");
    run_command("touch", "/tmp/note");
    run_command("append", "/tmp/note hello");
    run_command("/bin/cat.elf", "/tmp/note");
    run_command("/bin/ls.elf", "/tmp");
    run_command("rm", "/tmp/note");
    run_command("touch", "/tmp/words");
    run_command("append", "/tmp/words banana");
    run_command("append", "/tmp/words apple");
    run_command("append", "/tmp/words apple");
    run_command("append", "/tmp/words carrot");
    run_command("sort", "/tmp/words");
    run_command("uniq", "/tmp/words");
    run_command("uniq", "-c /tmp/words");
    run_command("uniq", "-d /tmp/words");
    run_command("uniq", "-u /tmp/words");
    run_command("/bin/find.elf", "/proc/sys");
    run_command("/bin/find.elf", "/mnt/boot/bin");
    run_command("touch", "/tmp/script");
    run_command("append", "/tmp/script pwd");
    run_command("append", "/tmp/script whoami");
    run_command("append", "/tmp/script '/bin/args.elf \"script words\" plain'");
    run_command("/bin/sh.elf", "-c whoami");
    run_command("/bin/sh.elf", "-c /bin/echo.elf sh-$LONGVAR");
    run_command("/bin/sh.elf", "-c /bin/echo.elf sh-env-$ROOT");
    run_command("/bin/sh.elf", "-c /bin/false.elf ; /bin/echo.elf sh-status-$?");
    run_command("/bin/sh.elf", "-c /bin/echo.elf sh-redir-ok > /tmp/sh-redir");
    run_command("/bin/sh.elf", "-c /bin/cat.elf < /tmp/sh-redir");
    run_command("/bin/sh.elf", "-c /bin/cat.elf /etc/os-release | wc");
    run_command("/bin/sh.elf", "/tmp/script");
    run_command("sh", "/tmp/script");
    run_command("/bin/duptest.elf");
    run_command("/bin/cat.elf", "/tmp/dup.txt");
    run_command("rm", "/tmp/dup.txt");
    char fds_redirect[64];
    copy_text(fds_redirect, sizeof(fds_redirect), "/bin/fds.elf > /tmp/fds.txt");
    run_shell_line(fds_redirect);
    run_command("/bin/cat.elf", "/tmp/fds.txt");
    run_command("rm", "/tmp/fds.txt");
    char lsof_pipeline[80];
    copy_text(lsof_pipeline, sizeof(lsof_pipeline), "/bin/echo.elf descriptor | /bin/lsof.elf | grep pipe");
    run_shell_line(lsof_pipeline);
    run_command("/bin/fdinh.elf");
    run_command("/bin/cat.elf", "/tmp/fdinherit.out");
    run_command("rm", "/tmp/fdinherit.in");
    run_command("rm", "/tmp/fdinherit.out");
    run_command("rm", "/tmp/script");
    run_command("rm", "/tmp/words");
    run_command("cp", "/etc/os-release /tmp/osrel");
    run_command("/bin/cat.elf", "/tmp/osrel");
    run_command("wc", "/tmp/osrel");
    run_command("mv", "/tmp/osrel /tmp/osrel2");
    run_command("/bin/ln.elf", "/tmp/osrel2 /tmp/osrel.link");
    run_command("/bin/stat.elf", "/tmp/osrel2");
    run_command("/bin/stat.elf", "/tmp/osrel.link");
    run_command("append", "/tmp/osrel.link LINKED");
    run_command("/bin/cat.elf", "/tmp/osrel2");
    run_command("rm", "/tmp/osrel.link");
    run_command("/bin/stat.elf", "/tmp/osrel2");
    run_command("touch", "/tmp/trunc");
    run_command("append", "/tmp/trunc abcdefghij");
    run_command("/bin/truncate.elf", "/tmp/trunc 4");
    run_command("/bin/stat.elf", "/tmp/trunc");
    run_command("/bin/cat.elf", "/tmp/trunc");
    run_command("/bin/truncate.elf", "/tmp/trunc 8");
    run_command("/bin/stat.elf", "/tmp/trunc");
    run_command("rm", "/tmp/trunc");
    run_command("grep", "VERSION /tmp/osrel2");
    char grep_pipeline[64];
    copy_text(grep_pipeline, sizeof(grep_pipeline), "/bin/cat.elf /etc/os-release | grep VERSION");
    run_shell_line(grep_pipeline);
    char tee_pipeline[96];
    copy_text(tee_pipeline, sizeof(tee_pipeline), "/bin/cat.elf /etc/os-release | tee /tmp/tee.txt | wc");
    run_shell_line(tee_pipeline);
    run_command("/bin/cat.elf", "/tmp/tee.txt");
    run_command("rm", "/tmp/tee.txt");
    char wc_pipeline[64];
    copy_text(wc_pipeline, sizeof(wc_pipeline), "/bin/cat.elf /etc/os-release | grep VERSION | wc");
    run_shell_line(wc_pipeline);
    char blocking_pipeline[80];
    copy_text(blocking_pipeline, sizeof(blocking_pipeline), "slowcat /etc/os-release | grep VERSION");
    run_shell_line(blocking_pipeline);
    char full_pipe_pipeline[64];
    copy_text(full_pipe_pipeline, sizeof(full_pipe_pipeline), "burst | wc");
    run_shell_line(full_pipe_pipeline);
    char big_file_pipeline[80];
    copy_text(big_file_pipeline, sizeof(big_file_pipeline), "burst | tee /tmp/big.txt | wc");
    run_shell_line(big_file_pipeline);
    run_command("wc", "/tmp/big.txt");
    run_command("rm", "/tmp/big.txt");
    char broken_pipe_pipeline[64];
    copy_text(broken_pipe_pipeline, sizeof(broken_pipe_pipeline), "burst | true");
    run_shell_line(broken_pipe_pipeline);
    char background_pipeline[64];
    copy_text(background_pipeline, sizeof(background_pipeline), "sleep 1 | wc &");
    run_shell_line(background_pipeline);
    run_command("jobs");
    run_command("wait", "%+");
    run_command("reap", "%+");
    run_command("jobs");
    char redirected[64];
    copy_text(redirected, sizeof(redirected), "/bin/echo.elf redirected output > /tmp/redirect.txt");
    run_shell_line(redirected);
    run_command("/bin/cat.elf", "/tmp/redirect.txt");
    char input_redirected[64];
    copy_text(input_redirected, sizeof(input_redirected), "/bin/cat.elf < /tmp/redirect.txt");
    run_shell_line(input_redirected);
    run_command("rm", "/tmp/redirect.txt");
    char append_first[64];
    copy_text(append_first, sizeof(append_first), "/bin/echo.elf first > /tmp/append.txt");
    run_shell_line(append_first);
    char append_second[64];
    copy_text(append_second, sizeof(append_second), "/bin/echo.elf second >> /tmp/append.txt");
    run_shell_line(append_second);
    run_command("/bin/cat.elf", "/tmp/append.txt");
    run_command("rm", "/tmp/append.txt");
    char stderr_redirected[64];
    copy_text(stderr_redirected, sizeof(stderr_redirected), "err 2> /tmp/stderr.txt");
    run_shell_line(stderr_redirected);
    char stderr_appended[64];
    copy_text(stderr_appended, sizeof(stderr_appended), "err 2>> /tmp/stderr.txt");
    run_shell_line(stderr_appended);
    run_command("/bin/cat.elf", "/tmp/stderr.txt");
    run_command("rm", "/tmp/stderr.txt");
    run_command("mkdir", "/tmp/work");
    run_command("cd", "/tmp/work");
    run_command("pwd");
    run_command("touch", "note");
    run_command("append", "note nested");
    run_command("/bin/cat.elf", "note");
    run_command("cd", "/tmp");
    run_command("rmdir", "work");
    run_command("rm", "/tmp/work/note");
    run_command("rmdir", "work");
    run_command("/bin/ls.elf", "/tmp");
    run_command("rm", "/tmp/osrel2");
    run_command("/bin/ls.elf", "/tmp");
    run_command("ps");
    run_command("mem");
    run_command("cpus");
    run_command("devices");
    run_command("fb");
    run_command("ticks");
    run_command("history");
    char chained[] = "echo chain-one ; echo chain-two";
    run_shell_line(chained);
    char status_chain[128];
    copy_text(status_chain, sizeof(status_chain), "false ; echo $? ; false && echo should-not-run || echo fallback-ran ; true && echo and-ran");
    run_shell_line(status_chain);
    shell_emit("[shell] boot shell complete");
    shell_trace_commands = false;
}

void run_shell_segment(char* line) {
    bool background = strip_background_suffix(line);
    for (char* scan = line; *scan != 0; ++scan) {
        if (*scan == '|') {
            run_pipeline_segment(line, background);
            return;
        }
    }
    char* command = nullptr;
    char* argument = nullptr;
    char* redirect = nullptr;
    char* input = nullptr;
    char* stderr_path = nullptr;
    bool append = false;
    bool stderr_append = false;
    if (!parse_command_fragment(line, command, argument, redirect, input, stderr_path, append, stderr_append)) return;
    char expanded_command[64];
    char expanded_argument[128];
    char expanded_redirect[64];
    char expanded_input[64];
    char expanded_stderr[64];
    expand_shell_variables(command, expanded_command, sizeof(expanded_command));
    expand_shell_variables(argument, expanded_argument, sizeof(expanded_argument));
    if (redirect) expand_shell_variables(redirect, expanded_redirect, sizeof(expanded_redirect));
    if (input) expand_shell_variables(input, expanded_input, sizeof(expanded_input));
    if (stderr_path) expand_shell_variables(stderr_path, expanded_stderr, sizeof(expanded_stderr));
    command = expanded_command;
    argument = expanded_argument;
    redirect = redirect ? expanded_redirect : nullptr;
    input = input ? expanded_input : nullptr;
    stderr_path = stderr_path ? expanded_stderr : nullptr;
    if (background) {
        for (uint64_t i = 0; i < sizeof(kCommands) / sizeof(kCommands[0]); ++i) {
            if (streq(command, kCommands[i].name)) {
                shell_line("bg: ", "builtins unsupported");
                shell_last_status = 126;
                return;
            }
        }
        if (run_background_command(command, argument, redirect, input, stderr_path, append, stderr_append)) return;
        shell_path("unknown command ", command);
        shell_last_status = 127;
        return;
    }
    run_command(command, argument, redirect, input, stderr_path, append, stderr_append);
}

void run_shell_line(char* line) {
    enum class Connector { Always, And, Or };
    char* cursor = line;
    Connector connector = Connector::Always;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') ++cursor;
        if (*cursor == 0) return;
        char* segment = cursor;
        Connector next_connector = Connector::Always;
        while (*cursor != 0) {
            if (*cursor == ';') {
                next_connector = Connector::Always;
                *cursor++ = 0;
                break;
            }
            if (*cursor == '&' && cursor[1] == '&') {
                next_connector = Connector::And;
                *cursor = 0;
                cursor += 2;
                break;
            }
            if (*cursor == '|' && cursor[1] == '|') {
                next_connector = Connector::Or;
                *cursor = 0;
                cursor += 2;
                break;
            }
            ++cursor;
        }
        bool should_run = connector == Connector::Always ||
            (connector == Connector::And && shell_last_status == 0) ||
            (connector == Connector::Or && shell_last_status != 0);
        if (should_run) {
            run_shell_segment(segment);
        }
        connector = next_connector;
    }
}

void run_recovery_line(char* line) {
    char* command = nullptr;
    char* argument = nullptr;
    split_recovery_command(line, command, argument);
    if (!command || command[0] == 0) return;
    if (streq(command, "help")) recovery_help();
    else if (streq(command, "status")) recovery_status();
    else if (streq(command, "check")) recovery_check();
    else if (streq(command, "logs")) recovery_logs();
    else if (streq(command, "mounts")) recovery_mounts();
    else if (streq(command, "files")) recovery_files();
    else if (streq(command, "processes")) recovery_processes();
    else if (streq(command, "hardware")) recovery_hardware();
    else if (streq(command, "clear")) command_clear(argument);
    else if (streq(command, "exit")) command_exit(argument);
    else if (streq(command, "shell")) {
        shell_emit("leaving recovery target; starting normal shell");
        shell_prompt_text = "ianos> ";
        interactive_shell_loop();
    } else if (recovery_passthrough_allowed(command)) {
        run_command(command, argument);
    } else {
        shell_path("recovery: unknown command ", command);
        shell_emit("type help for recovery commands, or shell for the normal environment");
        shell_last_status = 127;
    }
}

[[noreturn]] void recovery_shell_loop() {
    shell_emit("");
    shell_emit("IanOS Recovery Environment");
    shell_emit("minimal rescue target active; normal shell startup was not entered");
    shell_emit("type help for rescue commands, or shell to continue into normal userspace");
    shell_emit("");
    recovery_status();
    char line[kLineCapacity];
    char draft[kLineCapacity];
    uint64_t length = 0;
    uint64_t cursor = 0;
    uint64_t history_view = shell_history_count;
    line[0] = 0;
    draft[0] = 0;
    interactive_prompt();
    for (;;) {
        char input = 0;
        auto key = syscall(hybrid::SyscallNumber::Read, hybrid::kStdinFd, reinterpret_cast<uint64_t>(&input), 1);
        if (key.error != hybrid::kSyscallErrorNone) {
            asm volatile("pause");
            continue;
        }
        char c = input;
        if (c == 0x11) {
            terminal_scroll(-10);
        } else if (c == 0x12) {
            terminal_scroll(10);
        } else if (c == '\r' || c == '\n') {
            terminal_to_bottom();
            while (cursor < length) {
                echo_char(line[cursor]);
                ++cursor;
            }
            write_terminal("\n");
            line[length] = 0;
            history_push(line);
            run_recovery_line(line);
            length = 0;
            cursor = 0;
            line[0] = 0;
            draft[0] = 0;
            history_view = shell_history_count;
            interactive_prompt();
        } else if (c == 0x10) {
            terminal_to_bottom();
            if (shell_history_count != 0 && history_view != 0) {
                if (history_view == shell_history_count) copy_line(draft, line);
                --history_view;
                replace_input(line, length, cursor, history_at(history_view));
            }
        } else if (c == 0x0e) {
            terminal_to_bottom();
            if (history_view < shell_history_count) {
                ++history_view;
                replace_input(line, length, cursor, history_view == shell_history_count ? draft : history_at(history_view));
            }
        } else if (c == 0x04) {
            terminal_to_bottom();
            if (length == 0) {
                write_terminal("exit\n");
                syscall(hybrid::SyscallNumber::Exit, 0);
                for (;;) asm volatile("pause");
            }
            if (cursor < length) {
                for (uint64_t i = cursor; i < length; ++i) line[i] = line[i + 1];
                --length;
                redraw_input(line, length, cursor, cursor);
            }
        } else if (c == 0x0c) {
            write_terminal("\f");
            redraw_input(line, length, cursor, cursor);
        } else if (c == 0x01) {
            terminal_to_bottom();
            redraw_input(line, length, cursor, 0);
        } else if (c == 0x05) {
            terminal_to_bottom();
            redraw_input(line, length, cursor, length);
        } else if (c == 0x02) {
            terminal_to_bottom();
            if (cursor != 0) redraw_input(line, length, cursor, cursor - 1);
        } else if (c == 0x06) {
            terminal_to_bottom();
            if (cursor < length) redraw_input(line, length, cursor, cursor + 1);
        } else if (c == 0x7f) {
            terminal_to_bottom();
            if (cursor < length) {
                for (uint64_t i = cursor; i < length; ++i) line[i] = line[i + 1];
                --length;
                redraw_input(line, length, cursor, cursor);
            }
        } else if (c == '\t') {
            terminal_to_bottom();
            shell_emit("[complete] recovery commands: help status check logs mounts files processes hardware shell clear exit");
            redraw_input(line, length, cursor, cursor);
        } else if (c == '\b') {
            terminal_to_bottom();
            if (cursor != 0) {
                uint64_t target = cursor - 1;
                for (uint64_t i = target; i < length; ++i) line[i] = line[i + 1];
                --length;
                write_terminal("\b");
                --cursor;
                redraw_input(line, length, cursor, target);
            }
        } else if (c >= 32 && c < 127 && length + 1 < sizeof(line)) {
            terminal_to_bottom();
            for (uint64_t i = length + 1; i > cursor; --i) line[i] = line[i - 1];
            line[cursor] = c;
            ++length;
            line[length] = 0;
            redraw_input(line, length, cursor, cursor + 1);
            history_view = shell_history_count;
            copy_line(draft, line);
        }
    }
}

[[noreturn]] void interactive_shell_loop() {
    if (shell_tag_output) {
        shell_emit("[shell] interactive shell ready");
        shell_emit("[shell] type commands in the QEMU window and press Enter");
        shell_emit("[shell] interactive prompt enabled");
    } else {
        write_fastfetch_banner();
        shell_emit("IanOS shell ready");
        shell_emit("type commands in the QEMU window and press Enter");
    }
    char line[kLineCapacity];
    char draft[kLineCapacity];
    uint64_t length = 0;
    uint64_t cursor = 0;
    uint64_t history_view = shell_history_count;
    line[0] = 0;
    draft[0] = 0;
    interactive_prompt();
    for (;;) {
        char input = 0;
        auto key = syscall(hybrid::SyscallNumber::Read, hybrid::kStdinFd, reinterpret_cast<uint64_t>(&input), 1);
        if (key.error != hybrid::kSyscallErrorNone) {
            asm volatile("pause");
            continue;
        }
        char c = input;
        if (c == 0x11) {
            terminal_scroll(-10);
        } else if (c == 0x12) {
            terminal_scroll(10);
        } else if (c == '\r' || c == '\n') {
            terminal_to_bottom();
            while (cursor < length) {
                echo_char(line[cursor]);
                ++cursor;
            }
            write_terminal("\n");
            line[length] = 0;
            history_push(line);
            run_shell_line(line);
            length = 0;
            cursor = 0;
            line[0] = 0;
            draft[0] = 0;
            history_view = shell_history_count;
            interactive_prompt();
        } else if (c == 0x10) {
            terminal_to_bottom();
            if (shell_history_count != 0 && history_view != 0) {
                if (history_view == shell_history_count) copy_line(draft, line);
                --history_view;
                replace_input(line, length, cursor, history_at(history_view));
            }
        } else if (c == 0x0e) {
            terminal_to_bottom();
            if (history_view < shell_history_count) {
                ++history_view;
                replace_input(line, length, cursor, history_view == shell_history_count ? draft : history_at(history_view));
            }
        } else if (c == 0x04) {
            terminal_to_bottom();
            if (length == 0) {
                write_terminal("exit\n");
                syscall(hybrid::SyscallNumber::Exit, 0);
                for (;;) asm volatile("pause");
            }
            if (cursor < length) {
                for (uint64_t i = cursor; i < length; ++i) line[i] = line[i + 1];
                --length;
                redraw_input(line, length, cursor, cursor);
            }
        } else if (c == 0x0c) {
            write_terminal("\f");
            redraw_input(line, length, cursor, cursor);
        } else if (c == 0x01) {
            terminal_to_bottom();
            redraw_input(line, length, cursor, 0);
        } else if (c == 0x05) {
            terminal_to_bottom();
            redraw_input(line, length, cursor, length);
        } else if (c == 0x02) {
            terminal_to_bottom();
            if (cursor != 0) {
                redraw_input(line, length, cursor, cursor - 1);
            }
        } else if (c == 0x06) {
            terminal_to_bottom();
            if (cursor < length) {
                redraw_input(line, length, cursor, cursor + 1);
            }
        } else if (c == 0x7f) {
            terminal_to_bottom();
            if (cursor < length) {
                for (uint64_t i = cursor; i < length; ++i) line[i] = line[i + 1];
                --length;
                redraw_input(line, length, cursor, cursor);
            }
        } else if (c == '\t') {
            terminal_to_bottom();
            complete_input(line, length, cursor);
        } else if (c == '\b') {
            terminal_to_bottom();
            if (cursor != 0) {
                uint64_t target = cursor - 1;
                for (uint64_t i = target; i < length; ++i) line[i] = line[i + 1];
                --length;
                write_terminal("\b");
                --cursor;
                redraw_input(line, length, cursor, target);
            }
        } else if (c >= 32 && c < 127 && length + 1 < sizeof(line)) {
            terminal_to_bottom();
            for (uint64_t i = length + 1; i > cursor; --i) line[i] = line[i - 1];
            line[cursor] = c;
            ++length;
            line[length] = 0;
            redraw_input(line, length, cursor, cursor + 1);
            history_view = shell_history_count;
            copy_line(draft, line);
        }
    }
}

} // namespace

extern "C" [[noreturn]] void _start() {
    log("userland init entered");
    syscall(hybrid::SyscallNumber::GetThreadId);
    syscall(hybrid::SyscallNumber::GetProcessCount);
    syscall(hybrid::SyscallNumber::GetRunnableProcessCount);
    syscall(hybrid::SyscallNumber::GetLiveProcessCount);
    syscall(hybrid::SyscallNumber::GetExitedProcessCount);
    syscall(hybrid::SyscallNumber::GetUserThreadCount);
    syscall(hybrid::SyscallNumber::GetRunnableUserThreadCount);
    syscall(hybrid::SyscallNumber::GetCpuCount);
    hybrid::CpuInfo cpu_info;
    syscall(hybrid::SyscallNumber::GetCpuInfo, 0, reinterpret_cast<uint64_t>(&cpu_info));
    auto argument_count = syscall(hybrid::SyscallNumber::GetArgumentCount);
    if (argument_count.error == hybrid::kSyscallErrorNone && argument_count.value != 0) {
        hybrid::ArgumentInfo argument;
        syscall(hybrid::SyscallNumber::GetArgument, 0, reinterpret_cast<uint64_t>(&argument));
    }
    auto environment_count = syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (environment_count.error == hybrid::kSyscallErrorNone && environment_count.value != 0) {
        hybrid::EnvironmentInfo environment;
        syscall(hybrid::SyscallNumber::GetEnvironment, 0, reinterpret_cast<uint64_t>(&environment));
    }
    syscall(hybrid::SyscallNumber::GetDeviceCount);
    syscall(hybrid::SyscallNumber::GetStorageDeviceCount);
    syscall(hybrid::SyscallNumber::GetNetworkDeviceCount);
    syscall(hybrid::SyscallNumber::GetDisplayDeviceCount);
    hybrid::DeviceInfo device_info;
    syscall(hybrid::SyscallNumber::GetDeviceInfo, 0, reinterpret_cast<uint64_t>(&device_info));
    hybrid::DeviceInfo storage_info;
    hybrid::DeviceInfo network_info;
    hybrid::DeviceInfo display_info;
    syscall(hybrid::SyscallNumber::GetDeviceInfoByClass, static_cast<uint64_t>(hybrid::DeviceClass::Storage), 0, reinterpret_cast<uint64_t>(&storage_info));
    syscall(hybrid::SyscallNumber::GetDeviceInfoByClass, static_cast<uint64_t>(hybrid::DeviceClass::Network), 0, reinterpret_cast<uint64_t>(&network_info));
    syscall(hybrid::SyscallNumber::GetDeviceInfoByClass, static_cast<uint64_t>(hybrid::DeviceClass::Display), 0, reinterpret_cast<uint64_t>(&display_info));
    hybrid::FramebufferInfo framebuffer_info;
    syscall(hybrid::SyscallNumber::GetFramebufferInfo, reinterpret_cast<uint64_t>(&framebuffer_info));
    hybrid::MemoryStatsInfo memory_stats;
    syscall(hybrid::SyscallNumber::GetMemoryStats, reinterpret_cast<uint64_t>(&memory_stats));
    hybrid::SystemInfo system_info;
    syscall(hybrid::SyscallNumber::GetSystemInfo, reinterpret_cast<uint64_t>(&system_info));
    char kernel_log_sample[64];
    syscall(hybrid::SyscallNumber::ReadKernelLog, reinterpret_cast<uint64_t>(kernel_log_sample), sizeof(kernel_log_sample), 0);
    hybrid::ProcessInfo process_info;
    syscall(hybrid::SyscallNumber::GetProcessInfo, 0, reinterpret_cast<uint64_t>(&process_info));
    hybrid::UserThreadInfo thread_info;
    syscall(hybrid::SyscallNumber::GetUserThreadInfo, 0, reinterpret_cast<uint64_t>(&thread_info));
    hybrid::LaunchContextInfo launch_context;
    syscall(hybrid::SyscallNumber::GetLaunchContext, thread_info.tid, reinterpret_cast<uint64_t>(&launch_context));
    hybrid::SchedulerStatsInfo scheduler_stats;
    syscall(hybrid::SyscallNumber::GetSchedulerStats, reinterpret_cast<uint64_t>(&scheduler_stats));
    syscall(hybrid::SyscallNumber::ReapProcess, 0);
    auto node_count = syscall(hybrid::SyscallNumber::GetVfsNodeCount);
    if (node_count.error == hybrid::kSyscallErrorNone && node_count.value != 0) {
        hybrid::VfsNodeInfo node_info;
        syscall(hybrid::SyscallNumber::GetVfsNodeInfo, 0, reinterpret_cast<uint64_t>(&node_info));
    }
    syscall(hybrid::SyscallNumber::GetCurrentProcessId);
    hybrid::PathInfo cwd;
    syscall(hybrid::SyscallNumber::GetCurrentDirectory, reinterpret_cast<uint64_t>(&cwd));
    const char user_dir[] = "/user";
    syscall(hybrid::SyscallNumber::SetCurrentDirectory, reinterpret_cast<uint64_t>(user_dir), sizeof(user_dir));
    const char relative_init[] = "init.elf";
    auto relative_fd = syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(relative_init), sizeof(relative_init));
    if (relative_fd.error == hybrid::kSyscallErrorNone && relative_fd.value >= 3) {
        char relative_header[4];
        syscall(hybrid::SyscallNumber::Read, relative_fd.value, reinterpret_cast<uint64_t>(relative_header), sizeof(relative_header));
        syscall(hybrid::SyscallNumber::Close, relative_fd.value);
    }
    const char root_dir[] = "/";
    syscall(hybrid::SyscallNumber::SetCurrentDirectory, reinterpret_cast<uint64_t>(root_dir), sizeof(root_dir));
    const char init_path[] = "/user/init.elf";
    syscall(hybrid::SyscallNumber::VfsStat, reinterpret_cast<uint64_t>(init_path), sizeof(init_path));
    char header[4];
    syscall(hybrid::SyscallNumber::VfsRead, reinterpret_cast<uint64_t>(init_path), sizeof(init_path), reinterpret_cast<uint64_t>(header), sizeof(header));
    auto opened = syscall(hybrid::SyscallNumber::VfsOpen, reinterpret_cast<uint64_t>(init_path), sizeof(init_path));
    if (opened.error == hybrid::kSyscallErrorNone && opened.value != 0) {
        syscall(hybrid::SyscallNumber::VfsReadHandle, opened.value, reinterpret_cast<uint64_t>(header), sizeof(header));
        syscall(hybrid::SyscallNumber::VfsClose, opened.value);
    }
    auto fd = syscall(hybrid::SyscallNumber::Open, reinterpret_cast<uint64_t>(init_path), sizeof(init_path));
    if (fd.error == hybrid::kSyscallErrorNone && fd.value >= 3) {
        syscall(hybrid::SyscallNumber::Read, fd.value, reinterpret_cast<uint64_t>(header), sizeof(header));
        syscall(hybrid::SyscallNumber::Seek, fd.value, 0);
        syscall(hybrid::SyscallNumber::Read, fd.value, reinterpret_cast<uint64_t>(header), sizeof(header));
        syscall(hybrid::SyscallNumber::Close, fd.value);
    }
    ensure_shell_process_group();
    restore_shell_foreground_group();
    bool recovery = recovery_requested();
    if (recovery) enter_recovery_shell_mode();
    if (boot_script_requested()) run_boot_shell_script();
    if (recovery) recovery_shell_loop();
    interactive_shell_loop();
}
