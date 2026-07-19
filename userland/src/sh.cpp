#include "hybrid/user.hpp"

namespace {

constexpr uint64_t kLineCapacity = 128;
constexpr uint64_t kMaxPipelineStages = 4;

bool streq(const char* left, const char* right) {
    uint64_t i = 0;
    while (left[i] != 0 && right[i] != 0) {
        if (left[i] != right[i]) return false;
        ++i;
    }
    return left[i] == 0 && right[i] == 0;
}

bool get_arg(uint64_t index, hybrid::ArgumentInfo& out) {
    auto* bytes = reinterpret_cast<unsigned char*>(&out);
    for (uint64_t i = 0; i < sizeof(out); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetArgument, index, reinterpret_cast<uint64_t>(&out));
    return result.error == hybrid::kSyscallErrorNone;
}

uint64_t argument_count() {
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetArgumentCount);
    return count.error == hybrid::kSyscallErrorNone ? count.value : 0;
}

void copy_text(char* out, uint64_t capacity, const char* text) {
    if (!out || capacity == 0) return;
    uint64_t i = 0;
    if (text) {
        for (; i + 1 < capacity && text[i] != 0; ++i) out[i] = text[i];
    }
    out[i] = 0;
}

void append_decimal(char* buffer, uint64_t capacity, uint64_t& cursor, uint64_t value) {
    char digits[20];
    uint64_t count = 0;
    do {
        digits[count++] = static_cast<char>('0' + (value % 10));
        value /= 10;
    } while (value != 0 && count < sizeof(digits));
    while (count != 0) hybrid::user::append_char(buffer, capacity, cursor, digits[--count]);
}

bool variable_name_char(char c, bool first) {
    bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    bool digit = c >= '0' && c <= '9';
    return first ? alpha : (alpha || digit);
}

bool environment_value(const char* key, char* out, uint64_t capacity) {
    if (!key || !out || capacity == 0) return false;
    out[0] = 0;
    auto count = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironmentCount);
    if (count.error != hybrid::kSyscallErrorNone) return false;
    for (uint64_t i = 0; i < count.value; ++i) {
        hybrid::EnvironmentInfo environment;
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::GetEnvironment, i, reinterpret_cast<uint64_t>(&environment));
        if (result.error != hybrid::kSyscallErrorNone || !streq(environment.key, key)) continue;
        copy_text(out, capacity, environment.value);
        return out[0] != 0;
    }
    return false;
}

void append_environment_value(char* out, uint64_t capacity, uint64_t& cursor, const char* key, uint64_t key_length) {
    if (!key || key_length == 0) return;
    char name[24];
    if (key_length >= sizeof(name)) return;
    for (uint64_t i = 0; i < key_length; ++i) name[i] = key[i];
    name[key_length] = 0;
    char value[80];
    if (environment_value(name, value, sizeof(value))) hybrid::user::append_text(out, capacity, cursor, value);
}

void expand_shell_variables(const char* input, char* out, uint64_t capacity, uint64_t status) {
    if (!out || capacity == 0) return;
    out[0] = 0;
    if (!input) return;
    uint64_t cursor = 0;
    for (uint64_t i = 0; input[i] != 0; ++i) {
        if (input[i] != '$') {
            hybrid::user::append_char(out, capacity, cursor, input[i]);
            continue;
        }
        char next = input[i + 1];
        if (next == '?') {
            append_decimal(out, capacity, cursor, status);
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
        hybrid::user::append_char(out, capacity, cursor, '$');
    }
}

void write_error_line(const char* text) {
    hybrid::user::write_fd(hybrid::kStderrFd, text);
    hybrid::user::write_fd(hybrid::kStderrFd, "\n");
}

void trim(char*& text) {
    while (*text == ' ' || *text == '\t') ++text;
    char* end = text;
    while (*end != 0) ++end;
    while (end > text && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
}

bool stat_path(const char* path) {
    hybrid::VfsStatInfo info;
    auto* bytes = reinterpret_cast<unsigned char*>(&info);
    for (uint64_t i = 0; i < sizeof(info); ++i) bytes[i] = 0;
    auto result = hybrid::user::syscall(hybrid::SyscallNumber::VfsStatInfo,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1,
                                        reinterpret_cast<uint64_t>(&info));
    return result.error == hybrid::kSyscallErrorNone;
}

bool resolve_command(const char* name, char* out, uint64_t capacity) {
    if (!name || name[0] == 0) return false;
    if (name[0] == '/') {
        copy_text(out, capacity, name);
        return stat_path(out);
    }

    char candidate[64];
    uint64_t cursor = 0;
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, "/bin/");
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, name);
    if (stat_path(candidate)) {
        copy_text(out, capacity, candidate);
        return true;
    }

    cursor = 0;
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, "/bin/");
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, name);
    hybrid::user::append_text(candidate, sizeof(candidate), cursor, ".elf");
    if (!stat_path(candidate)) return false;
    copy_text(out, capacity, candidate);
    return true;
}

bool create_output_file(const char* path, bool append) {
    if (!path || path[0] == 0) return false;
    if (append) {
        auto exists = hybrid::user::syscall(hybrid::SyscallNumber::VfsStat,
                                            reinterpret_cast<uint64_t>(path),
                                            hybrid::user::strlen(path) + 1);
        if (exists.error == hybrid::kSyscallErrorNone) return true;
    } else {
        hybrid::user::syscall(hybrid::SyscallNumber::DeleteFile,
                              reinterpret_cast<uint64_t>(path),
                              hybrid::user::strlen(path) + 1);
    }
    auto created = hybrid::user::syscall(hybrid::SyscallNumber::CreateFile,
                                         reinterpret_cast<uint64_t>(path),
                                         hybrid::user::strlen(path) + 1);
    return created.error == hybrid::kSyscallErrorNone;
}

bool spawn_child(
    char* command,
    char* argument,
    const char* stdout_path,
    const char* stdin_path,
    const char* stderr_path,
    bool stdout_append,
    bool stderr_append,
    uint32_t stdin_pipe,
    uint32_t stdout_pipe,
    uint64_t& out_pid,
    bool start_after_setup = true) {
    char path[64];
    if (!resolve_command(command, path, sizeof(path))) {
        hybrid::user::write_fd(hybrid::kStderrFd, "[sh] not found ");
        hybrid::user::write_fd(hybrid::kStderrFd, command);
        hybrid::user::write_fd(hybrid::kStderrFd, "\n");
        return false;
    }

    char line[128];
    uint64_t cursor = 0;
    hybrid::user::append_text(line, sizeof(line), cursor, path);
    if (argument && argument[0] != 0) {
        hybrid::user::append_char(line, sizeof(line), cursor, ' ');
        hybrid::user::append_text(line, sizeof(line), cursor, argument);
    }

    uint64_t pid = 0;
    auto spawned = hybrid::user::syscall(hybrid::SyscallNumber::Spawn,
                                         reinterpret_cast<uint64_t>(line),
                                         hybrid::user::strlen(line) + 1,
                                         reinterpret_cast<uint64_t>(&pid),
                                         hybrid::SpawnFlagStartSuspended);
    if (spawned.error != hybrid::kSyscallErrorNone || pid == 0) return false;
    out_pid = pid;

    if (stdin_pipe != 0) {
        auto attached = hybrid::user::syscall(hybrid::SyscallNumber::AttachPipeFd,
                                             pid,
                                             hybrid::kStdinFd,
                                             stdin_pipe,
                                             static_cast<uint64_t>(hybrid::PipeEndpoint::Read));
        if (attached.error != hybrid::kSyscallErrorNone) return false;
    } else if (stdin_path && stdin_path[0] != 0) {
        auto redirected = hybrid::user::syscall(hybrid::SyscallNumber::RedirectProcessFd,
                                                pid,
                                                hybrid::kStdinFd,
                                                reinterpret_cast<uint64_t>(stdin_path),
                                                hybrid::user::strlen(stdin_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) return false;
    }

    if (stdout_pipe != 0) {
        auto attached = hybrid::user::syscall(hybrid::SyscallNumber::AttachPipeFd,
                                             pid,
                                             hybrid::kStdoutFd,
                                             stdout_pipe,
                                             static_cast<uint64_t>(hybrid::PipeEndpoint::Write));
        if (attached.error != hybrid::kSyscallErrorNone) return false;
    } else if (stdout_path && stdout_path[0] != 0) {
        if (!create_output_file(stdout_path, stdout_append)) return false;
        auto redirect_call = stdout_append ? hybrid::SyscallNumber::RedirectProcessFdAppend : hybrid::SyscallNumber::RedirectProcessFd;
        auto redirected = hybrid::user::syscall(redirect_call,
                                                pid,
                                                hybrid::kStdoutFd,
                                                reinterpret_cast<uint64_t>(stdout_path),
                                                hybrid::user::strlen(stdout_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) return false;
    }

    if (stderr_path && stderr_path[0] != 0) {
        if (!create_output_file(stderr_path, stderr_append)) return false;
        auto redirect_call = stderr_append ? hybrid::SyscallNumber::RedirectProcessFdAppend : hybrid::SyscallNumber::RedirectProcessFd;
        auto redirected = hybrid::user::syscall(redirect_call,
                                                pid,
                                                hybrid::kStderrFd,
                                                reinterpret_cast<uint64_t>(stderr_path),
                                                hybrid::user::strlen(stderr_path) + 1);
        if (redirected.error != hybrid::kSyscallErrorNone) return false;
    }
    if (start_after_setup) {
        auto started = hybrid::user::syscall(hybrid::SyscallNumber::StartProcess, pid);
        if (started.error != hybrid::kSyscallErrorNone) return false;
    }
    return true;
}

uint64_t wait_reap(uint64_t pid) {
    uint64_t code = 0;
    for (;;) {
        auto waited = hybrid::user::syscall(hybrid::SyscallNumber::Wait, pid, reinterpret_cast<uint64_t>(&code));
        if (waited.error == hybrid::kSyscallErrorNone) break;
        hybrid::user::syscall(hybrid::SyscallNumber::Yield);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::ReapProcess, pid);
    return code;
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
    trim(fragment);
    if (fragment[0] == 0) return false;
    command = fragment;
    while (*fragment != 0 && *fragment != ' ' && *fragment != '\t') ++fragment;
    if (*fragment != 0) *fragment++ = 0;
    trim(fragment);
    argument = fragment;
    stdout_path = nullptr;
    stdin_path = nullptr;
    stderr_path = nullptr;
    stdout_append = false;
    stderr_append = false;

    for (char* scan = argument; *scan != 0; ++scan) {
        bool fd2 = false;
        if (*scan == '2' && scan[1] == '>') fd2 = true;
        else if (*scan != '>' && *scan != '<') continue;

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
        if (*scan == 0) return false;
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
        if (*scan != 0) *scan = 0;
    }
    char* end = argument;
    while (*end != 0) ++end;
    while (end > argument && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
    return true;
}

uint64_t run_external(
    char* command,
    char* argument,
    char* stdout_path,
    char* stdin_path,
    char* stderr_path,
    bool stdout_append,
    bool stderr_append) {
    uint64_t pid = 0;
    if (!spawn_child(command, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append, 0, 0, pid)) return 126;

    uint64_t code = wait_reap(pid);
    return code;
}

bool has_pipeline(char* segment) {
    for (char* scan = segment; *scan != 0; ++scan) {
        if (*scan == '|' && scan[1] != '|') return true;
    }
    return false;
}

uint64_t run_pipeline(char* line, uint64_t last_status) {
    char* stages[kMaxPipelineStages];
    uint64_t stage_count = 0;
    char* cursor = line;
    while (stage_count < kMaxPipelineStages) {
        trim(cursor);
        if (*cursor == 0) break;
        stages[stage_count++] = cursor;
        while (*cursor != 0 && *cursor != '|') ++cursor;
        if (*cursor == '|') *cursor++ = 0;
    }
    if (stage_count < 2) return 126;
    uint32_t pipes[kMaxPipelineStages - 1];
    uint64_t pids[kMaxPipelineStages];
    for (uint64_t i = 0; i < kMaxPipelineStages - 1; ++i) pipes[i] = 0;
    for (uint64_t i = 0; i < kMaxPipelineStages; ++i) pids[i] = 0;

    for (uint64_t i = 0; i + 1 < stage_count; ++i) {
        auto pipe = hybrid::user::syscall(hybrid::SyscallNumber::CreatePipe);
        if (pipe.error != hybrid::kSyscallErrorNone || pipe.value == 0) return 126;
        pipes[i] = static_cast<uint32_t>(pipe.value);
    }

    for (uint64_t i = 0; i < stage_count; ++i) {
        char* command = nullptr;
        char* argument = nullptr;
        char* stdout_path = nullptr;
        char* stdin_path = nullptr;
        char* stderr_path = nullptr;
        bool stdout_append = false;
        bool stderr_append = false;
        if (!parse_command_fragment(stages[i], command, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append)) return 126;
        char expanded_command[64];
        char expanded_argument[128];
        char expanded_stdout[64];
        char expanded_stdin[64];
        char expanded_stderr[64];
        expand_shell_variables(command, expanded_command, sizeof(expanded_command), last_status);
        expand_shell_variables(argument, expanded_argument, sizeof(expanded_argument), last_status);
        if (stdout_path) expand_shell_variables(stdout_path, expanded_stdout, sizeof(expanded_stdout), last_status);
        if (stdin_path) expand_shell_variables(stdin_path, expanded_stdin, sizeof(expanded_stdin), last_status);
        if (stderr_path) expand_shell_variables(stderr_path, expanded_stderr, sizeof(expanded_stderr), last_status);
        command = expanded_command;
        argument = expanded_argument;
        stdout_path = stdout_path ? expanded_stdout : nullptr;
        stdin_path = stdin_path ? expanded_stdin : nullptr;
        stderr_path = stderr_path ? expanded_stderr : nullptr;
        uint32_t input_pipe = stdin_path ? 0 : (i == 0 ? 0 : pipes[i - 1]);
        uint32_t output_pipe = (i + 1 == stage_count) ? 0 : pipes[i];
        if (!spawn_child(command,
                         argument,
                         (i + 1 == stage_count) ? stdout_path : nullptr,
                         stdin_path,
                         stderr_path,
                         (i + 1 == stage_count) && stdout_append,
                         stderr_append,
                         input_pipe,
                         output_pipe,
                         pids[i],
                         false)) {
            return 126;
        }
    }

    uint64_t final_status = 0;
    for (uint64_t i = 0; i < stage_count; ++i) {
        auto started = hybrid::user::syscall(hybrid::SyscallNumber::StartProcess, pids[i]);
        if (started.error != hybrid::kSyscallErrorNone) return 126;
    }
    for (uint64_t i = 0; i < stage_count; ++i) final_status = wait_reap(pids[i]);
    for (uint64_t i = 0; i + 1 < stage_count; ++i) hybrid::user::syscall(hybrid::SyscallNumber::ClosePipe, pipes[i]);
    return final_status;
}

uint64_t run_segment(char* segment, uint64_t last_status) {
    trim(segment);
    if (segment[0] == 0 || segment[0] == '#') return last_status;
    if (has_pipeline(segment)) return run_pipeline(segment, last_status);

    char* command = nullptr;
    char* argument = nullptr;
    char* stdout_path = nullptr;
    char* stdin_path = nullptr;
    char* stderr_path = nullptr;
    bool stdout_append = false;
    bool stderr_append = false;
    if (!parse_command_fragment(segment, command, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append)) return 126;
    char expanded_command[64];
    char expanded_argument[128];
    char expanded_stdout[64];
    char expanded_stdin[64];
    char expanded_stderr[64];
    expand_shell_variables(command, expanded_command, sizeof(expanded_command), last_status);
    expand_shell_variables(argument, expanded_argument, sizeof(expanded_argument), last_status);
    if (stdout_path) expand_shell_variables(stdout_path, expanded_stdout, sizeof(expanded_stdout), last_status);
    if (stdin_path) expand_shell_variables(stdin_path, expanded_stdin, sizeof(expanded_stdin), last_status);
    if (stderr_path) expand_shell_variables(stderr_path, expanded_stderr, sizeof(expanded_stderr), last_status);
    command = expanded_command;
    argument = expanded_argument;
    stdout_path = stdout_path ? expanded_stdout : nullptr;
    stdin_path = stdin_path ? expanded_stdin : nullptr;
    stderr_path = stderr_path ? expanded_stderr : nullptr;

    if (streq(command, "exit")) return argument[0] == 0 ? last_status : 0;
    if (streq(command, "cd")) {
        auto result = hybrid::user::syscall(hybrid::SyscallNumber::SetCurrentDirectory,
                                            reinterpret_cast<uint64_t>(argument),
                                            hybrid::user::strlen(argument) + 1);
        return result.error == hybrid::kSyscallErrorNone ? 0 : 1;
    }
    return run_external(command, argument, stdout_path, stdin_path, stderr_path, stdout_append, stderr_append);
}

uint64_t run_line(char* line, uint64_t last_status) {
    enum class Connector { Always, And, Or };
    Connector connector = Connector::Always;
    char* cursor = line;
    for (;;) {
        while (*cursor == ' ' || *cursor == '\t' || *cursor == ';') ++cursor;
        if (*cursor == 0 || *cursor == '#') return last_status;
        char* segment = cursor;
        Connector next = Connector::Always;
        while (*cursor != 0) {
            if (*cursor == ';') {
                next = Connector::Always;
                *cursor++ = 0;
                break;
            }
            if (*cursor == '&' && cursor[1] == '&') {
                next = Connector::And;
                *cursor = 0;
                cursor += 2;
                break;
            }
            if (*cursor == '|' && cursor[1] == '|') {
                next = Connector::Or;
                *cursor = 0;
                cursor += 2;
                break;
            }
            ++cursor;
        }
        bool should_run = connector == Connector::Always ||
            (connector == Connector::And && last_status == 0) ||
            (connector == Connector::Or && last_status != 0);
        if (should_run) last_status = run_segment(segment, last_status);
        connector = next;
    }
}

uint64_t run_script_file(const char* path) {
    auto opened = hybrid::user::syscall(hybrid::SyscallNumber::Open,
                                        reinterpret_cast<uint64_t>(path),
                                        hybrid::user::strlen(path) + 1);
    if (opened.error != hybrid::kSyscallErrorNone || opened.value < 3) {
        write_error_line("[sh] open failed");
        return 2;
    }

    uint64_t status = 0;
    char line[kLineCapacity];
    uint64_t length = 0;
    for (;;) {
        char byte = 0;
        auto read = hybrid::user::syscall(hybrid::SyscallNumber::Read, opened.value, reinterpret_cast<uint64_t>(&byte), 1);
        if (read.error != hybrid::kSyscallErrorNone || read.value == 0) break;
        if (byte == '\r') continue;
        if (byte == '\n') {
            line[length] = 0;
            status = run_line(line, status);
            length = 0;
            continue;
        }
        if (length + 1 < sizeof(line)) line[length++] = byte;
    }
    if (length != 0) {
        line[length] = 0;
        status = run_line(line, status);
    }
    hybrid::user::syscall(hybrid::SyscallNumber::Close, opened.value);
    return status;
}

} // namespace

extern "C" [[noreturn]] void _start() {
    hybrid::ArgumentInfo first;
    hybrid::ArgumentInfo second;
    uint64_t status = 0;
    if (!get_arg(1, first)) {
        write_error_line("[sh] usage: sh -c <line> | sh <script>");
        hybrid::user::exit(2);
    }

    if (streq(first.value, "-c")) {
        uint64_t count = argument_count();
        if (count < 3) {
            write_error_line("[sh] missing -c command");
            hybrid::user::exit(2);
        }
        char line[kLineCapacity];
        line[0] = 0;
        uint64_t cursor = 0;
        for (uint64_t i = 2; i < count; ++i) {
            if (!get_arg(i, second)) continue;
            if (cursor != 0) hybrid::user::append_char(line, sizeof(line), cursor, ' ');
            hybrid::user::append_text(line, sizeof(line), cursor, second.value);
        }
        status = run_line(line, 0);
    } else {
        status = run_script_file(first.value);
    }
    hybrid::user::exit(status);
}
