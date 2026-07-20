# Userland Foundation

IanOS builds freestanding user payloads as internal artifacts, links
them at `0x400000`, copies them into the ESP staging tree, and includes them in
the build manifest before packaging `build/out/image/kernel.img`. The linked
payloads also remain under `build/out` for inspection, while copied payloads
remain under `build/esp` for QEMU image generation. The custom bootloader passes
them to the kernel as boot modules so VFS can expose `/user/init.elf` plus
external command images under `/bin`.

## ABI

Syscall numbers and result codes live in `common/include/hybrid/syscall.hpp` so
kernel and user binaries share the same contract. The current user program uses
`int 0x80` wrappers for:

- debug logging
- PIT tick reads
- CMOS RTC date/time reads for `date`
- cooperative yield
- sleep by scheduler ticks; `SleepTicks` blocks the current CPL3 user thread
  until a timer tick wakes it instead of busy-spinning inside the syscall
- current thread ID
- process count
- thread count
- runnable process count
- exited process count
- user thread count
- runnable user thread count
- startup argument and environment counts plus indexed reads
- process environment mutation through `SetEnvironment(key, value)` and `UnsetEnvironment(key)`, with fixed 24-byte keys and 80-byte values
- process and user-thread snapshots, including user-thread block reasons and wait targets
- current PID and per-process current directory get/set
- launch-context export for the init thread
- scheduler runtime statistics
- CPU topology count and per-CPU APIC/enabled/online/bootstrap/startup-attempt/parked/scheduler/descriptor-ready/Local-APIC-timer-ready/bootstrap-work/fixed-IPI-work flags
- PMM memory statistics
- framebuffer discovery
- device inventory enumeration
- boot block-device statistics and raw 512-byte sector reads
- mount count and indexed mount records for filesystem discovery
- VFS node count and indexed node info for diagnostics, plus direct directory-entry reads for normal `ls`-style namespace traversal
- VFS size-only stat, path-based stat metadata, read/open/read-handle/close
- process-local open/read/seek/close, including cwd-relative open
- process-local `Dup(fd)` and `Dup2(oldfd, newfd)` for shared-handle descriptor duplication
- process-local descriptor table snapshots through `GetFileDescriptorInfo(pid, index, out)`, including same-parent sibling inspection for shell-launched external tools
- live pipe-table snapshots through `GetPipeCount` and `GetPipeInfo(index, out)`
- kernel-backed `/dev/null`, `/dev/zero`, `/dev/tty`, and `/dev/console` character-device reads/writes through normal file descriptors
- default fd 0, fd 1, and fd 2 descriptors backed by `/dev/tty` for newly created processes, with normal redirection and inheritance able to replace them
- generated `/proc/meminfo`, `/proc/iomem`, `/proc/buddyinfo`, `/proc/heapinfo`, `/proc/vmstat`, `/proc/uptime`, `/proc/loadavg`, `/proc/sched_debug`, `/proc/stat`, `/proc/block/bootdisk`, `/proc/driver/summary`, `/proc/driver/devices`, `/proc/pci/summary`, `/proc/pci/devices`, `/proc/irq/summary`, `/proc/interrupts`, `/proc/tty/summary`, `/proc/cpu/summary`, `/proc/cpu/topology`, `/proc/processes`, `/proc/mounts`, `/proc/filesystems`, `/proc/fs/vfs`, `/proc/cmdline`, `/proc/self/status`, `/proc/self/stat`, `/proc/self/maps`, `/proc/self/cmdline`, `/proc/self/environ`, `/proc/self/cwd`, `/proc/self/exe`, `/proc/self/root`, `/proc/self/fd`, `/proc/self/fdinfo`, `/proc/self/limits`, `/proc/self/task`, `/proc/<pid>/status`, `/proc/<pid>/stat`, `/proc/<pid>/maps`, `/proc/<pid>/cmdline`, `/proc/<pid>/environ`, `/proc/<pid>/cwd`, `/proc/<pid>/exe`, `/proc/<pid>/root`, `/proc/<pid>/fd`, `/proc/<pid>/fdinfo`, `/proc/<pid>/limits`, `/proc/<pid>/task`, and `/proc/<pid>/task/<tid>/{status,stat}` virtual-file reads through normal file descriptors
- `/proc/self/fd/N` and `/proc/<pid>/fd/N` link resolution through `ReadLink(path, out, capacity)`
- RAM VFS create/write/delete file operations
- RAM VFS create/delete directory operations with parent-directory validation and non-empty directory refusal
- fd-style terminal input/output through `Read(0, ...)` and `Write(1/2, ...)`
- terminal viewport control through `TerminalControl(ScrollRelative/ScrollToBottom)`
- process-scoped stdin/stdout/stderr redirection through `RedirectProcessFd(pid, fd, path)`
- append-mode child stdout/stderr redirection through `RedirectProcessFdAppend(pid, fd, path)`
- `Spawn(command_line, out_pid)` for creating a runnable process record from a VFS-backed ELF64 image and bounded quote-aware argv entries up to 64 bytes each
- `Exit(code)`, signal-style `Kill(pid, SIGTERM/SIGKILL)`, `Wait(pid, out_code)`, `WaitAny(out_info)`, and `ReapProcess(pid)` for process lifecycle control
- user-scheduler snapshots with schedulable counts and timeslice quantum metadata, next-runnable user-thread launch-context selection, interrupt-frame backed user `Yield`, Local APIC timer-driven user preemption, and scheduler-backed process waits

## Shell

`init.elf` contains the first userland shell dispatcher. It runs at CPL3 after
the kernel enters the init launch context with `iretq`. Normal QEMU launches go
through the custom UEFI boot manager, then drop into the interactive `ianos> `
shell. Recovery launches are selected in that same boot manager; the loader sets
`kBootFlagRecovery`, the kernel passes `--recovery`, and init switches to the
dedicated `recovery# ` rescue target instead of entering the normal shell. That
target prints startup status, exposes repair-oriented commands (`status`,
`check`, `logs`, `mounts`, `files`, `processes`, `hardware`), allows selected
read-only diagnostic passthrough commands, and requires an explicit `shell`
command to continue into the normal shell. Verification launches with an
explicit boot-test marker, runs a boot-scripted command sequence first, then
switches into the same interactive keyboard-driven loop. Normal interactive output is prefixless; the
`[shell]` prefix is reserved for the automated boot-script transcript so
`verify.ps1` can validate specific shell events. The dispatcher is real userland
code and uses only the shared syscall ABI. Unknown command names are resolved
through the shell `PATH` by appending `.elf`, then using `Spawn`,
scheduler-maintained user preemption, kernel-blocking `Wait`, and `ReapProcess`.
The shell `run` command now uses the same scheduler-backed wait path against the
last spawned PID instead of the older bounded `RunProcess` handoff.

Built-in commands:

- `help`
- `clear`
- `history`
- `exit`
- `echo`
- `status`
- `pid`
- `ctx`
- `argv`
- `env`
- `export`
- `unset`
- `which`
- `stat`
- `counts`
- `spawn`
- `jobs`
- `usched`
- `nextuser`
- `run`
- `kill`
- `wait`
- `reap`
- `pwd`
- `cd`
- `ls`
- `cat`
- `sh`
- `fds`
- `ps`
- `mem`
- `cpus`
- `devices`
- `fb`
- `ticks`

The `ids` built-in calls `GetCurrentIds` and prints the current user PID, current
user TID, parent PID, current kernel scheduler thread ID, and current CPU ID.
The `ctx` built-in calls `GetCurrentUserContext` and prints the current PID,
active user TID, process state, thread state, saved user RIP/RSP, and CR3. These
give scheduler and process-control tools syscall-visible current identity and
current execution-context contracts instead of requiring them to infer context
from the older active PID scalar.

The boot script produces serial shell output during QEMU verification and now
launches `/bin/hello.elf alpha beta` through `Spawn`, asks the kernel which user
thread it would dispatch next, runs it through the scheduler-backed `run`/`Wait`
path, then exercises `Wait` and `ReapProcess`, proving that the shell can ask
the kernel to validate an ELF image, create a second runnable user process with
argv metadata, block on the child, switch into it, resume the shell context,
observe the exit code, and clean up the process record. It also runs `hello gamma delta`
and `args one two` through external command resolution, proving the shell can
execute multiple `/bin` programs by command name rather than only through the
low-level lifecycle commands. The script also runs `/bin/cat.elf /bin/args.elf`
to prove an external user program can open, read, and close a process-local file
descriptor.
It also runs `/bin/ls.elf /bin` to prove an external user program can enumerate
direct directory entries through syscalls, while diagnostic tools can still use
whole-tree VFS node metadata. `/bin/blk.elf` proves userspace
can read boot-disk cache statistics and LBA0 through the block-device syscalls.
It also runs `/bin/mount.elf`, which reads the kernel mount table and prints
the boot-module VFS root plus the read-only FAT16 `/mnt/boot` disk mount.
`/bin/df.elf` consumes the same mount records to report per-mount node counts
and byte totals from userspace. `/bin/lsblk.elf` combines the boot block-device
stats with the mount table to show the initialized boot disk, sector geometry,
cache/read counters, and the disk-backed `/mnt/boot` mountpoint.
`/bin/du.elf` walks syscall-visible VFS node records under a normalized path and
splits subtree usage into disk-backed and memory-backed bytes.
`/bin/pipeinfo.elf` reads the kernel pipe table and reports live pipe capacity,
queued bytes, read offset, and reader/writer endpoint counts for pipeline
debugging.
`/bin/devio.elf` proves the `/dev` character-device layer by reading 32 zero
bytes from `/dev/zero` and discarding a payload through `/dev/null`.
`/bin/tty.elf` proves terminal-backed character devices by querying the current
terminal input mode, opening `/dev/tty` and `/dev/console` as normal files,
checking their readable idle state, and writing through those descriptors.
`/bin/ttystat.elf` streams `/proc/tty/summary` through a raw VFS handle so
scrollback, input-mode, queue, and framebuffer console counters are available
as a normal command.
`/bin/stty.elf` reports the current terminal input mode and switches between
raw and canonical mode through the terminal-control syscall.
`/bin/ttyread.elf` switches to canonical mode, injects a deterministic cooked
line into the terminal input discipline, reads it back through fd 0 backed by
`/dev/tty`, and restores raw mode for the interactive shell.
The boot proof also stats, lists, and reads generated `/proc/meminfo`,
`/proc/iomem`, `/proc/buddyinfo`, `/proc/heapinfo`, `/proc/vmstat`, `/proc/uptime`, `/proc/loadavg`, `/proc/sched_debug`, `/proc/block/bootdisk`, `/proc/driver/summary`, `/proc/driver/devices`, `/proc/pci/summary`, `/proc/pci/devices`, `/proc/irq/summary`, `/proc/interrupts`, `/proc/tty/summary`, `/proc/cpu/summary`, `/proc/cpu/topology`, `/proc/processes`, `/proc/mounts`, `/proc/filesystems`,
`/proc/fs/vfs`, `/proc/cmdline`, `/proc/stat`, `/proc/sys/kernel/hostname`, `/proc/sys/kernel/ostype`,
`/proc/sys/kernel/osrelease`, `/proc/sys/kernel/cpus`, `/proc/sys/kernel/online_cpus`, `/proc/sys/kernel/cpu_online_mask`, `/proc/sys/kernel/cpu_scheduler_mask`, `/proc/sys/kernel/cpu_parked_mask`, `/proc/sys/kernel/boot_mode`, `/proc/sys/kernel/boot_flags`, `/proc/sys/kernel/boot_options`, `/proc/sys/kernel/machine`, `/proc/sys/kernel/modules`, `/proc/sys/kernel/features`, `/proc/sys/kernel/abi_version`, `/proc/sys/kernel/version`, `/proc/self/status`,
`/proc/self/stat`, `/proc/self/maps`, `/proc/self/cmdline`, `/proc/self/environ`, `/proc/self/cwd`, `/proc/self/exe`, `/proc/self/root`, `/proc/self/fd`, `/proc/self/fdinfo`, `/proc/self/limits`, `/proc/1/status`, `/proc/1/stat`, `/proc/1/maps`, `/proc/1/cmdline`, `/proc/1/environ`, `/proc/1/cwd`, `/proc/1/exe`, `/proc/1/root`, `/proc/1/fd`, `/proc/1/fdinfo`, and `/proc/1/limits` through the same VFS and
file-descriptor syscalls used by normal
commands. The fd-table proof shows fd 0, fd 1, and fd 2 resolving to `/dev/tty`
unless a command has redirected them. `ls /proc` now includes numeric process directories and `ls /proc/1`
exposes generated `status`, `stat`, `maps`, and `fd` entries for the init shell process.
`/bin/readlink.elf` redirects stdout to `/tmp/readlink.txt`, resolves
`/proc/self/fd/1`, resolves the same descriptor through its numeric
`/proc/<pid>/fd/1` path, enumerates its numeric `/proc/<pid>/fd` directory, and
then reads the captured result back, proving fd link targets are derived from
the running process descriptor table.
`/bin/realpath.elf` canonicalizes relative or absolute paths, rejects missing
targets through VFS metadata lookup, and resolves `/proc/self/fd/N` links before
printing the final normalized path.
`/bin/kill.elf` uses the same process-control syscalls as the shell built-in to
send SIGTERM/SIGKILL-style termination to a PID, or to a process group through
`--pgid`.
`/bin/killall.elf` enumerates process records and sends the same
SIGTERM/SIGKILL-style termination to live processes matching a full path,
basename, or basename without `.elf`.
`/bin/pgrep.elf` uses the same matching rules to print live matching PIDs and
PGIDs without signaling them. `/bin/pidof.elf` prints matching PIDs in
Linux-like plain output while also exposing verifier-tagged match counts.
`/bin/nproc.elf` reports the scheduler-visible
online CPU count through the same syscall used by the shell `cpus` built-in.
`/bin/groups.elf` reads the current process identity context and reports the
active root primary group for the current single-user model.
`/bin/lscpu.elf` reads the CPU topology syscall and reports discovered CPU IDs,
APIC IDs, ACPI processor IDs, readiness flags, online count, parked APs,
scheduler-ready CPUs, and Local APIC timer readiness.
`/bin/schedstat.elf` reads the kernel scheduler statistics syscall and reports
thread-state counts, switch/yield/preempt totals, current scheduler thread, and
current CPU metadata. `/bin/scheddebug.elf` streams `/proc/sched_debug`
through ordinary VFS open/read syscalls and reports scheduler totals together
with userspace block/wake/preemption counters.
`/bin/vmstat.elf` combines memory, tick, and scheduler statistics into one
syscall-backed virtual-memory and scheduler activity snapshot.
`/bin/top.elf` takes a deterministic top-style snapshot of process, thread,
memory, scheduler, and CPU accounting using existing kernel syscalls, making it
usable in the boot verifier and from the interactive shell.
`/bin/pstree.elf` reads the process table and reports parent/child hierarchy
depth, child counts, state, and process names for a Linux-like process-tree
view.
`/bin/findmnt.elf` reads the kernel mount table and reports target, source,
filesystem type, flags, node count, and byte usage for all mounts or a filtered
mount path.
`/bin/mountinfo.elf` streams `/proc/self/mountinfo` by default, or
`/proc/mountinfo` with `all`/`-a`, using the kernel-rendered Linux-shaped
mountinfo file for the current single mount namespace.
`/bin/iostat.elf` reads the boot block-device statistics syscall and reports
sector reads, derived read bytes, cache hits and misses, cache fills, entries,
request size, last LBA, reject counters, backend failures, and hit percentage.
`/bin/lsmem.elf` reads the physical-memory statistics syscall and reports the
usable physical range, page totals, usable and reserved bytes, highest physical
address, and used percentage.
`/bin/fbset.elf` reads the framebuffer-info syscall and reports the GOP base,
geometry, scanline pitch, pixel size, pixel format, and color masks.
`/bin/lspci.elf` reads the indexed device-inventory syscall and reports each
PCI-backed storage, network, and display device with BDF, vendor/device IDs,
required command bits, resource count, and MMIO/IO resources.
`/bin/irqstat.elf` reads `/proc/irq/summary` and selected `/proc/interrupts` rows through the
VFS syscalls and reports interrupt dispatch totals, APIC vectors, syscall
vector activity, and interrupt-table rows from userspace.
`/bin/mmstat.elf` reads `/proc/mm/summary` through the VFS syscalls and reports
PMM, VMM, and heap accounting from userspace.
`/bin/buddyinfo.elf` streams `/proc/buddyinfo` through ordinary VFS open/read
syscalls and reports allocatable free PMM runs by page order from the bitmap
allocator.
`/bin/heapinfo.elf` streams `/proc/heapinfo` through ordinary VFS open/read
syscalls and reports kernel heap block totals, allocation/free counters,
failure counters, peak use, and last allocation size.
`/bin/procvmstat.elf` streams `/proc/vmstat` through ordinary VFS open/read
syscalls and reports Linux-like PMM/VMM activity counters.
`/bin/procstat.elf` streams `/proc/stat` through ordinary VFS open/read syscalls
and reports Linux-like CPU ticks, interrupt totals, selected vector counters,
context-switch, process, and boot-time counters.
`/bin/netstat.elf` reads `/proc/net/summary` and `/proc/net/dev` through the VFS
syscalls and reports e1000 link state, ring readiness, RX/TX counters, and
interface rows from userspace.
`/bin/lsmod.elf` reads `/proc/modules` through the VFS syscalls and reports the
bootloader-supplied kernel and userland module table from userspace.
`/bin/modinfo.elf` parses the same table and reports a selected boot module's
name, path, size, and loaded address by either basename or full module path.
`/bin/pmap.elf` uses process metadata syscalls to report a process entry point,
address-space root, image mapping, user stack mapping, owned page count, and
open descriptor count. Without arguments it reports the current process; with a
PID argument it reports that process.
`/bin/maps.elf` reads `/proc/self/maps` or `/proc/<pid>/maps` through ordinary
VFS file descriptors and prints the kernel-generated mapping rows plus byte
count, proving the procfs path is usable outside kernel metadata syscalls.
`/bin/pcmdline.elf` reads `/proc/self/cmdline` or `/proc/<pid>/cmdline`;
`/bin/proccomm.elf` reads `/proc/self/comm` or `/proc/<pid>/comm`;
`/bin/procenv.elf` reads `/proc/self/environ` or `/proc/<pid>/environ`;
`/bin/procwd.elf` reads `/proc/self/cwd` or `/proc/<pid>/cwd`;
`/bin/procexe.elf` reads `/proc/self/exe` or `/proc/<pid>/exe`;
`/bin/procroot.elf` reads `/proc/self/root` or `/proc/<pid>/root`;
`/bin/procfdinfo.elf` reads `/proc/self/fdinfo`, `/proc/<pid>/fdinfo`, or
`/proc/<pid>/fdinfo/<fd>`; `/bin/proclimits.elf` reads
`/proc/self/limits` or `/proc/<pid>/limits` through the same descriptor path,
including process, user-thread, fd, address-space, and heap capacity rows;
`/bin/procio.elf` reads `/proc/self/io` or `/proc/<pid>/io`, exposing per-process
read/write syscall and byte counters gathered by the kernel fd layer;
`/bin/proctask.elf` lists `/proc/self/task` or `/proc/<pid>/task` and streams
`/proc/<pid>/task/<tid>/status`, proving scheduler-owned process and thread
metadata is visible as Linux-like procfs files. `/proc/self/status` and
`/proc/<pid>/status` report their `Threads` value from the same per-PID task
table used by those directories.
`/bin/version.elf` cross-checks `GetSystemInfo` against
`/proc/sys/kernel/osrelease` and `/proc/sys/kernel/version` so release metadata
is visible through one command and validated across both ABI paths.
`/bin/limits.elf` reads the kernel limit ABI and reports fixed capacities for
boot modules, VFS tables, process slots, user-thread slots, process descriptors,
pipes, CPU slots, PMM bitmap coverage, and boot FAT path tracking alongside
selected live counts.
`/bin/imginfo.elf` inspects the `/mnt/boot` FAT mount from userspace, reporting
mounted payload bytes, node count, boot module count, and key boot artifact
sizes so image contents can be distinguished from the fixed host disk image.
`/bin/abi.elf` calls the kernel ABI-info syscall and reports the syscall ABI
version, maximum syscall number, BootInfo version, and shared userspace/kernel
structure sizes so layout drift is visible from a normal user process.
The same layout contract is exposed through `/proc/abi`, giving shell tools a
plain VFS path for ABI version and structure-size checks.
`/bin/features.elf` reads the kernel feature-info syscall and reports the
stable subsystem capability bitmap plus named feature rows for boot, memory,
VFS, userspace, scheduler, SMP/APIC, PCI/storage/network, and terminal support.
The same stable feature set is also exposed as `/proc/features` so ordinary
VFS reads can inspect kernel capability state without using the feature syscall
directly.
`/bin/kmsg.elf` reads `/proc/kmsg` through ordinary VFS open/read syscalls and
streams the retained kernel ring buffer with byte and line counters, while
`/bin/dmesg.elf` continues to exercise the direct kernel-log syscall.
`/bin/uptime.elf` reports raw scheduler ticks through the syscall ABI and also
opens `/proc/uptime` to prove the VFS-backed uptime summary is readable from
ordinary userspace.
`/bin/loadavg.elf` reads `/proc/loadavg` through ordinary VFS open/read syscalls
and reports the Linux-shaped runnable/live-process load snapshot from userspace.
`/bin/fastfetch.elf` reads the system, memory,
framebuffer, CPU, and device inventory syscalls to print the same compact IanOS
and Mattas identity summary that appears when the normal shell starts.
`/bin/sysctl.elf` reads the kernel sysctl-style virtual files under
`/proc/sys/kernel` and supports both `sysctl -a` and single-key reads such as
`sysctl kernel.osrelease`; process and thread capacity limits are exposed as
`kernel.pid_max` and `kernel.threads-max` from the same constants that size the
kernel userspace tables. `kernel.cpus` and `kernel.online_cpus` expose the
discovered and currently online CPU topology counts. `kernel.cpu_online_mask`,
`kernel.cpu_scheduler_mask`, and `kernel.cpu_parked_mask` expose runtime CPU
state as hex bitmasks, and `kernel.boot_mode` mirrors the retained boot flags.
`kernel.boot_flags` exposes the same flag bitmask in hex for boot diagnostics,
while `kernel.boot_options` renders the active boot flags as names.
`kernel.machine` exposes the target machine string used by `uname`.
`kernel.modules` exposes the retained boot-module count used by `/proc/modules`.
`kernel.features` exposes the stable kernel feature bitmask used by `/proc/features`.
`kernel.abi_version` exposes the shared syscall ABI version for compatibility checks.
`/bin/find.elf` walks syscall-visible VFS nodes below
a requested root path, including virtual `/proc` trees and disk-backed
`/mnt/boot` files, and prints typed path rows.
`/bin/lsattr.elf` reports the syscall-visible VFS attribute bits for files,
directories, virtual files, character devices, memory-backed nodes, and
disk-backed nodes.
`/bin/namei.elf` walks each absolute path component with VFS metadata, proving
mountpoint and nested directory resolution before the final file.
`/bin/tree.elf` recursively walks directory entries through `ReadDirectory` with
bounded depth and entry limits for safe filesystem inspection.
`/bin/statfs.elf` resolves a path to its longest matching mounted filesystem
record and reports mount path, type, source, flags, node count, and bytes.
`/bin/hexdump.elf` opens a VFS path through the process-local descriptor table
and prints bounded 16-byte hex/ASCII rows, boot-proven against
`/disk/bootsector.bin` and `/etc/os-release`.
`/bin/od.elf` reads path-backed or stdin-backed descriptor streams and prints
octal offsets with byte values in either octal (`-t o1`) or hex (`-t x1`).
`/bin/base64.elf` streams descriptor-backed input through Base64 encode mode
or whitespace-tolerant `-d` decode mode.
`/bin/which.elf` resolves command names through the process `PATH` environment
and reports the matching executable VFS path.
`/bin/printenv.elf` prints all inherited environment entries or a single named
entry, proving selected environment lookup from a normal external process.
`/bin/cal.elf` renders a month calendar from `month year` arguments or the
kernel RTC date when no arguments are supplied.
`/bin/readelf.elf` reads an ELF64 file header through the same descriptor path
and prints class, endian, type, machine, entry point, and table offsets/counts
for both memory-backed user programs and disk-backed kernel images.
`/bin/file.elf` combines VFS metadata with a bounded content prefix to classify
directories, character devices, virtual text, ELF64 x86_64 executables, ASCII
text, empty files, and binary data.
`/bin/sha256sum.elf` implements SHA-256 in freestanding userspace and hashes
path-backed or stdin-backed descriptor streams, with boot verification against
stable identity files.
`/bin/sha224sum.elf` implements SHA-224 with the standard SHA-224 initial state
and 224-bit truncated SHA-256-family digest.
`/bin/sha512sum.elf` implements SHA-512 in freestanding userspace and hashes
the same descriptor-backed streams for stronger compatibility diagnostics.
`/bin/sha384sum.elf` uses the SHA-512 block function with the SHA-384 initial
state and digest truncation, matching the standard SHA-2 variant.
`/bin/sha1sum.elf` implements SHA-1 in freestanding userspace and hashes the
same descriptor-backed streams for compatibility diagnostics.
`/bin/md5sum.elf` implements MD5 in freestanding userspace and hashes the same
path-backed or stdin-backed descriptor streams for compatibility diagnostics.
`/bin/cat.elf` accepts stdin or one or more path arguments, opening each path in
order through process-local descriptors and streaming text files to stdout.
`/bin/wc.elf` also accepts stdin or one or more path arguments, prints per-path
byte, line, and word counts, and prints an aggregate `total` row for multiple
paths. `/bin/head.elf` and `/bin/tail.elf` accept the default three-line mode
plus `-n count` or `-ncount` to bound output from a path or inherited stdin.
`/bin/cksum.elf` computes CRC32 over descriptor-backed streams, boot-proven
against `/etc/hostname`.
`/bin/fold.elf` wraps descriptor-backed text at a bounded column width and is
boot-proven against `/proc/version`.
`/bin/printf.elf` emits formatted script output with bounded `%s`, `%d`, `%x`,
`%c`, and escape handling.
`/bin/dd.elf` copies descriptor-backed input to writable VFS output with
bounded `if=`, `of=`, `bs=`, and `count=` operands.
`/bin/xargs.elf` reads whitespace-delimited stdin tokens, appends them to a
command line, spawns the child process, waits for it, and reaps it.
`/bin/yes.elf` emits repeated text with `count=N` or `-n N` bounds for
deterministic scripts while retaining a conservative default limit for manual
interactive runs.
`/bin/cmp.elf` opens two process-local descriptors at once and reports either
the matched byte count or the first differing byte, boot-proving both equal and
different file paths.
`/bin/strings.elf` extracts bounded printable runs from descriptor-backed files,
including identity text and kernel version virtual files.
`/bin/nl.elf` numbers line-oriented descriptor input, boot-proven against
`/etc/os-release`.
`/bin/tr.elf` translates byte streams using two bounded character sets and is
boot-proven against `/etc/os-release`.
`/bin/sed.elf` applies a bounded `s/from/to/` substitution over descriptor input
and is boot-proven against `/etc/os-release`.
`/bin/cut.elf` extracts a one-based delimiter-selected field from descriptor
input and is boot-proven against `/etc/os-release`.
`/bin/paste.elf` reads two descriptor-backed files line-by-line and emits
tab-joined rows, boot-proven against matching hostname files.
`/bin/rev.elf` reverses descriptor-backed input lines and is boot-proven against
`/etc/hostname`.
`/bin/tac.elf` emits descriptor-backed lines in reverse line order and is
boot-proven against `/etc/os-release`.
`/bin/seq.elf` emits bounded decimal sequences and is boot-proven with a
three-number range.
`/bin/expr.elf` evaluates bounded integer arithmetic and comparison expressions,
boot-proven with an addition expression.
The shell proof also stats and reads
`/disk/bootsector.bin`, which is registered from an AHCI READ DMA EXT of LBA0,
and stats/lists/reads `/mnt/boot/kernel.elf` plus nested files such as
`/mnt/boot/bin/hello.elf`, which are loaded through the recursive FAT16 reader
on the AHCI-backed disk image. The command set now includes
filesystem manipulation programs: `touch`, `append`, `rm`, `cp`, `mv`, `ln`, `truncate`, `wc`,
`grep`, `mkdir`, `rmdir`, `stat`, `statfs`, `whoami`, `hostname`, `id`, `basename`, `dirname`, `head`,
`tail`, `test`, `sort`, `uniq`, `/bin/find.elf`, `/bin/hexdump.elf`, `/bin/od.elf`, `/bin/base64.elf`, `/bin/which.elf`, `/bin/printenv.elf`, `/bin/cal.elf`, `/bin/readelf.elf`, `/bin/file.elf`, `/bin/lsattr.elf`, `/bin/namei.elf`, `/bin/tree.elf`, `/bin/statfs.elf`, `/bin/sha256sum.elf`, `/bin/sha224sum.elf`, `/bin/sha512sum.elf`, `/bin/sha384sum.elf`, `/bin/sha1sum.elf`, `/bin/md5sum.elf`, `/bin/cksum.elf`, `/bin/fold.elf`, `/bin/printf.elf`, `/bin/dd.elf`, `/bin/xargs.elf`, `/bin/yes.elf`, `/bin/cmp.elf`, `/bin/strings.elf`, `/bin/nl.elf`, `/bin/tr.elf`, `/bin/sed.elf`, `/bin/cut.elf`, `/bin/paste.elf`, `/bin/rev.elf`, `/bin/tac.elf`, `/bin/seq.elf`, `/bin/expr.elf`, `/bin/sh.elf`, `/bin/duptest.elf`, `/bin/fds.elf`, `/bin/lsof.elf`, `/bin/fdinh.elf`, `/bin/ln.elf`, `/bin/readlink.elf`, `/bin/realpath.elf`, `/bin/truncate.elf`, `/bin/pipeinfo.elf`, `/bin/maps.elf`, `/bin/pcmdline.elf`, `/bin/proccomm.elf`, `/bin/procenv.elf`, `/bin/procwd.elf`, `/bin/procexe.elf`, `/bin/procroot.elf`, `/bin/procfdinfo.elf`, `/bin/proclimits.elf`, `/bin/procio.elf`, `/bin/proctask.elf`, `/bin/fastfetch.elf`, `/bin/sysctl.elf`, `/bin/lsblk.elf`, `/bin/findmnt.elf`, `/bin/mountinfo.elf`, `/bin/iostat.elf`, `/bin/lsmem.elf`, `/bin/iomem.elf`, `/bin/fbset.elf`, `/bin/lspci.elf`, `/bin/irqstat.elf`, `/bin/interrupts.elf`, `/bin/mmstat.elf`, `/bin/buddyinfo.elf`, `/bin/heapinfo.elf`, `/bin/procvmstat.elf`, `/bin/procstat.elf`, `/bin/netstat.elf`, `/bin/lsmod.elf`, `/bin/modinfo.elf`, `/bin/kmsg.elf`, `/bin/loadavg.elf`, `/bin/scheddebug.elf`, `/bin/devio.elf`, `/bin/tty.elf`, `/bin/ttystat.elf`, `/bin/stty.elf`, `/bin/ttyread.elf`, `/bin/clear.elf`, `/bin/kill.elf`, `/bin/pgrep.elf`, `/bin/pidof.elf`, `/bin/nproc.elf`, `/bin/lscpu.elf`, `/bin/schedstat.elf`, `/bin/vmstat.elf`, `/bin/top.elf`, `/bin/pstree.elf`, `/bin/groups.elf`, `/bin/killall.elf`, and a diagnostic `err` program that writes
separately to stdout and stderr. `uniq` supports adjacent duplicate
suppression plus `-c` count-prefix, `-d` duplicate-only, and `-u`
unique-only modes over each emitted run. Built-in and external `stat` report normalized
path, type, size, link count, and VFS flags, while `statfs` reports the
filesystem record that owns a path. `/bin/ln.elf` creates hard links
between writable RAM-backed files; the boot proof links `/tmp/osrel2` to
`/tmp/osrel.link`, verifies both names report link count `2`, writes through
the alias, reads the new content through the original path, unlinks the alias,
and verifies the original remains with link count `1`. `/bin/truncate.elf`
shrinks or extends writable RAM-backed files and reports the resulting size
through shared VFS metadata. `/bin/mv.elf` uses the kernel rename syscall for
writable RAM-backed nodes and falls back to copy/delete when the target cannot
be renamed in place. RAM-backed writable VFS files now support up to 4 KiB per
file with sixteen live RAM-file slots, and verification stores the full
`/bin/burst.elf` 4096-byte stream through `tee /tmp/big.txt` before reading its
size back with `wc /tmp/big.txt`. The built-in `fds` and external `/bin/fds.elf`
report descriptor numbers, kind, VFS handle, shared offset, pipe id, and path
for live process descriptor slots. `/bin/ps.elf` reports process rows including
PGIDs, syscall counts, last syscall numbers, Local-APIC user runtime ticks,
user-scheduler switch counts, and APIC preempt counts plus user thread scheduler
rows with block reasons, pipe/process wait targets, syscall accounting, runtime
ticks, switch counts, and preempt counts.
`test` is intentionally quiet and reports
success through its exit status so shell `&&`/`||` chains can act like simple
scripts. The shell also has a bounded `sh` built-in that reads a VFS file and
executes each non-empty line through the same parser used by the interactive
prompt. `/bin/sh.elf` is a separate quiet user process that runs `-c` strings or
script files by spawning child programs through the shared process syscalls;
successful dispatch writes only command output, while errors remain prefixed for
diagnosis. It also handles stdin/stdout/stderr redirection and external-command
pipelines using the same descriptor and pipe syscalls as the interactive shell. Process
argv storage now keeps up to eight 64-byte arguments so shell-launched `-c` command
lines can carry small redirection and pipeline forms without truncation. The
kernel `Spawn` parser preserves single-quoted, double-quoted, and
backslash-escaped spaces as part of one argv entry before the child process
starts, and the boot proof exercises that through `/bin/args.elf` directly and
from a script file.
`/bin/duptest.elf` exercises the descriptor table directly: it duplicates a VFS
fd, writes through the duplicate, duplicates the original onto stdout, writes
through fd `1`, and leaves `/tmp/dup.txt` for the boot script to read back.
The boot proof also runs `/bin/fds.elf > /tmp/fds.txt` and reads that file back,
proving a redirected child can observe its inherited stdout descriptor as a VFS
fd through the shared descriptor-info syscall. `/bin/lsof.elf` walks visible
process descriptor tables, and the boot proof runs it inside a pipeline so it
reports inherited pipe-read and pipe-write endpoints.
`/bin/fdinh.elf` proves VFS stdio inheritance shares an open handle: the
parent duplicates a VFS file onto stdin, spawns `/bin/cat.elf`, and observes its
own inherited stdin offset advance after the child reads the shared descriptor.
The boot proof creates `/tmp/work`, changes the shell CWD into it, runs
external child commands against relative path `note`, verifies that `rmdir work`
refuses a non-empty directory, removes the file, then removes the directory.
`date` reads the kernel RTC syscall and prints a zero-padded
`YYYY-MM-DD HH:MM:SS` timestamp.

Spawned child processes inherit the parent shell's current directory and
environment, so relative paths and variables behave like users expect from a
shell session instead of falling back to fixed defaults. Userspace resolves
cwd-relative paths, and the VFS itself normalizes `.`, `..`, and repeated
separators before namespace lookup or RAM-file mutation. Child
stdin/stdout/stderr inherit from the parent by default, with explicit shell
redirection overriding those descriptors before execution. Kernel child
execution now saves the parent user execution context, activates the child
thread context, and restores the parent context on child `Exit`, instead of
tracking only a loose active PID. The shell exposes
environment mutation through `export KEY=value`, `unset KEY`, and `which`. Init starts with
`PATH=/bin`; verification resolves `grep` to `/bin/grep.elf`, exports
`EDITOR=hksh`, exports a longer `LONGVAR`, observes both in `/bin/env.elf`,
expands the longer value in the init shell and `/bin/sh.elf`, then unsets
`EDITOR` before continuing.
`kill` accepts `-9`/`KILL` and `-15`/`TERM`, stores the termination reason in
the process record, and `ps` reports both reason and exit code. Boot
verification kills spawned sleep children through both paths: SIGKILL-style
termination returns status `137`, and SIGTERM-style termination returns status
`143`. The shell also keeps a bounded local job table for spawned children;
`jobs` lists tracked job IDs, child PIDs, kernel process state, termination
reason, exit code, and command text. `fg [pid|%job|%+]` waits for a tracked
background child through the scheduler-backed `Wait` path and reaps it,
`bg [pid|%job|%+]` resumes a stopped job in the background, `stop [pid|%job|%+]`
uses the same kernel stopped-state path as Ctrl+Z for deterministic scripts,
`wait [pid|%job|%+]` waits for a specific job, `wait -n` waits for whichever
child exits first through `WaitAny` (`wait -a` is accepted as an alias), and
`reap [pid|%job|%+]` removes completed jobs from that table. Jobs can now hold
up to four member PIDs, so foreground and background pipelines are tracked under one PGID while
their stages are waited and reaped as a unit. External commands and external-command pipelines can be launched
in the background with a trailing `&`; the shell records the job while the kernel scheduler maintains user preemption
because the prompt does not block in `Wait`.
`/etc/hostname` is a kernel-owned rootfs file, and `/proc/sys/kernel/hostname`
exposes the same hostname through procfs. `/proc/sys/kernel/ostype`,
`/proc/sys/kernel/osrelease`, `/proc/sys/kernel/pid_max`,
`/proc/sys/kernel/threads-max`, `/proc/sys/kernel/cpus`,
`/proc/sys/kernel/online_cpus`, `/proc/sys/kernel/cpu_online_mask`,
`/proc/sys/kernel/cpu_scheduler_mask`, `/proc/sys/kernel/cpu_parked_mask`,
`/proc/sys/kernel/boot_mode`,
`/proc/sys/kernel/boot_flags`, `/proc/sys/kernel/boot_options`,
`/proc/sys/kernel/machine`, `/proc/sys/kernel/modules`,
`/proc/sys/kernel/features`, `/proc/sys/kernel/abi_version`, and
`/proc/sys/kernel/version` expose kernel identity, boot mode, boot flags,
machine type, CPU topology counts and masks, boot-module count, feature flags, ABI
version, and scheduler capacity in the same sysctl-style tree.
`/proc/heapinfo` exposes kernel heap block and allocation diagnostics. `/proc/vmstat` exposes Linux-like PMM and VMM counters. `/proc/buddyinfo` exposes PMM
free-run availability by page order from the current bitmap allocator.
`/proc/uptime` exposes raw ticks, PIT frequency, whole seconds, LAPIC tick
state, scheduler switch/yield/preemption counters, and live userspace load.
`/proc/loadavg` exposes an instantaneous runnable/live process load snapshot.
`/proc/sched_debug` exposes kernel scheduler totals and userspace
blocking/preemption diagnostics.
`/proc/stat` exposes scheduler, interrupt, and
process counters in a Linux-like text layout, `/proc/block/bootdisk` exposes
boot-disk cache counters, `/proc/driver/summary` exposes driver-manager
lifecycle counters, `/proc/driver/devices` exposes bound driver-device rows,
`/proc/pci/summary` exposes PCI scan and ECAM diagnostics,
`/proc/pci/devices` exposes retained device rows, `/proc/irq/summary` exposes
interrupt dispatch, vector, Local APIC timer, and user-preemption counters,
`/proc/interrupts` exposes a Linux-like per-vector interrupt table, `/bin/interrupts.elf` streams it directly through VFS, and
`/proc/tty/summary` exposes terminal input, scrollback, and framebuffer console
counters, `/proc/net/summary` plus `/proc/net/dev` expose e1000 link and
interface counters, and `/proc/cpu/summary` plus `/proc/cpu/topology` expose CPU startup,
runtime, parked-AP, IPI-work, and TLB-shootdown counters.
`/proc/modules` exposes the bootloader-provided module table, `/proc/cmdline`
exposes the boot command line, `/proc/kmsg`
exposes the retained kernel log ring through the VFS, and `/proc/fs/vfs`
exposes RAM-backed VFS mutation counters. `/bin/hostname.elf` reads the
rootfs file back as the system hostname. `/bin/id.elf` reports the fixed root uid/gid plus
the syscall-backed process, thread, and process-group IDs in a Linux-like format.
`/bin/ids.elf` exercises the `GetCurrentIds` syscall from a child process and
prints its user PID, user TID, parent PID, backing kernel thread ID, and current
CPU ID.
`/bin/ctx.elf` exercises `GetCurrentUserContext` from a normal child process and
prints its current PID/TID, process/thread state, saved RIP/RSP, and CR3.
`/bin/tee.elf` reads inherited stdin, writes inherited stdout, and mirrors the
same stream into a writable VFS file, including when it is the middle process in
a pipeline.
Process records now also carry a process-group ID. New children default
to their own PGID, the shell groups pipeline stages under the first stage's PID,
and `KillProcessGroup` applies SIGTERM/SIGKILL-style termination to all caller-
owned live members of that group.

Long-running commands rely on the kernel's process/thread lifecycle paths to
maintain user preemption while they wait and reap children. The boot image
includes `/bin/loop.elf`, a yielding long-running test
process used to verify `loop &`, `jobs`, `kill TERM %+`, `wait %+`, and
`reap %+`, `stop %+`, `bg %+`, and also verifies `sleep 5 &` followed by `fg %+` plus a runnable
`loop &` sibling while `sleep 5 &` is collected by `wait -n`.
The PS/2 keyboard layer now translates Ctrl-letter chords, and the shell polls
for Ctrl+C while waiting on a foreground child through `run`, `fg`, or a normal
external command. The shell sets a kernel-visible terminal foreground process
group around foreground external commands, `fg`, and foreground pipelines,
restores its own PGID afterward, and exposes the current terminal PGID through
`fgpgid`. Ctrl+C during foreground waits is delivered as SIGTERM through
`KillProcessGroup`, so single commands and foreground pipelines follow the same
process-group path. Ctrl+Z stops the foreground process group through
`StopProcessGroup`; `bg` and `fg` resume stopped groups through
`ContinueProcessGroup`.
External child exit codes are retained as the shell status. `status` and
`echo $?` report that value, `&&` runs the next command only after status `0`,
and `||` runs the next command only after a non-zero status. The init shell and
`/bin/sh.elf` expand `$?`, `$VAR`, and `${VAR}` in command arguments and
redirection paths before dispatch, so script lines can use the last status,
current environment, and PATH-derived child environments in the same form as
interactive commands. Verification runs
`false ; echo $? ; false && echo should-not-run || echo fallback-ran ; true &&
echo and-ran` and rejects the skipped branch if it appears.
The shell also supports simple stdio redirection for external commands with
`>`, `>>`, `<`, `2>`, and `2>>`. Verification runs
`/bin/echo.elf redirected output > /tmp/redirect.txt`, then reads the file back
with `/bin/cat.elf /tmp/redirect.txt` to prove fd `1` was attached to a VFS file
for that child process rather than the terminal. It also runs
`/bin/cat.elf < /tmp/redirect.txt`, where `cat` has no path argument and reads
the same text from redirected fd `0`.
Append redirection uses the same child fd table but seeks the redirected output
fd to EOF before execution; boot verification writes `first` with `>`, appends
`second` with `>>`, and reads both lines back from `/tmp/append.txt`.
Stderr redirection targets fd `2` independently; boot verification runs
`err 2> /tmp/stderr.txt` and `err 2>> /tmp/stderr.txt`, leaves `err` stdout on
the terminal, then reads the captured stderr lines from the file.
The shell now also supports external-command pipelines with `|`. Pipeline
stages are separate spawned user processes connected through bounded kernel pipe
objects attached to child fd `0` and fd `1`. The shell creates all pipe objects,
spawns all stages, assigns them to one process group, records a multi-process job,
and enables Local APIC user preemption. Foreground pipelines wait
for every stage through the job-control path, reap them, and release the pipe buffers. Background pipelines leave
the job table entry live so `jobs`, `wait %+`, and `reap %+` can manage the whole PGID later. Empty pipe reads while
a writer is still live and full pipe writes while a reader is still live now
mark the current user thread blocked on that pipe and switch the syscall return
frame to another runnable user thread. Peer writes wake blocked readers; peer
reads compact the pipe buffer and wake blocked writers. Empty pipe reads now
resume the blocked syscall directly with a copied byte count once a peer write
adds data, and full pipe writes resume with a copied byte count once a peer read
frees capacity. Verification includes `burst | wc`, where `burst` writes more
than the fixed pipe capacity without yielding, forcing a kernel-blocked writer
to resume after the reader drains data. Verification also runs
`burst | tee /tmp/big.txt | wc`, proving a 4 KiB RAM-backed file can receive a
full streaming pipeline capture, and `burst | true`;
once `true` exits and the read endpoint disappears, the blocked writer resumes
with a broken-pipe style syscall error instead of silently buffering data for a
pipe with no readers. Once the final write endpoint disappears, blocked readers
resume with EOF instead of remaining parked on a pipe that cannot receive more
data. Transfers larger than one resumed page chunk and full
POSIX pipe options remain future work.
Verification runs `/bin/cat.elf /etc/os-release | grep VERSION` and
`/bin/cat.elf /etc/os-release | grep VERSION | wc`, proving that `cat`,
`grep`, and `wc` can cooperate through redirected stdout/stdin instead of only
path arguments. `grep <pattern>` and `wc` now read fd `0` when no path argument
is provided.
Manual interactive command entry uses fd `0`, which the kernel backs with the
PS/2 keyboard IRQ buffer. The shell displays an `ianos> ` prompt, echoes printable
characters, handles cursor editing with Left/Right/Home/End/Delete/Backspace,
recalls command history with Up/Down, scrolls the framebuffer scrollback with
PageUp/PageDown, offers basic Tab completion, clears/redraws the prompt with
Ctrl-L, exits an empty prompt with Ctrl-D, preserves the current draft while
walking history, and runs commands on Enter. Shell output uses fd `1` through
the terminal `Write` syscall rather than debug logging. The lower-level
`ReadKey` syscall remains available for diagnostics and compatibility.

## Boot Handoff

The custom UEFI bootloader reads `\user\init.elf` before `ExitBootServices()` and
passes its physical base and size through `BootInfo`. The PMM reserves that
range so normal allocations cannot reuse the image buffer.

## Kernel Loader

`UserspaceManager::create_process_from_elf()` validates ELF64 headers, allocates
a user PML4, maps each PT_LOAD page with user PTE flags, copies file bytes,
zeros BSS space, maps a four-page user stack, applies writable and NX page
permissions from ELF segment flags, and records PID, entry, PML4, image base,
image page count, and stack top.

The process table also supports PID lookup plus created, runnable, and exited
state transitions. ELF-backed processes receive a main user-thread record with a
TID, owning PID, entry point, user stack pointer, PML4 root, runnable state, and
startup arguments/environment entries. Spawned child processes inherit the
parent PID, CWD, environment, and standard file descriptors, then receive their
parsed command-line argv. VFS stdio descriptors are reopened at the parent's
tracked offset; pipe endpoints are copied into the child descriptor table.
The boot path marks the loaded init process and its main thread runnable after
validating entry and stack mappings.

`UserspaceManager::build_launch_context()` converts a runnable user thread into
the state used by the ring-3 entry stub: RIP, RSP, CR3, user CS/SS selectors,
and interrupt-enabled RFLAGS. The same state is exported through the shared
syscall ABI as `LaunchContextInfo`, so init and diagnostic tools can inspect the
exact entry frame the kernel uses.

The manager can also save the currently active user thread's interrupted frame
and full general-purpose register context back into that thread record. The
syscall interrupt path uses this for cooperative CPL3 `Yield`: it saves the
outgoing shell frame, selects another runnable user thread, restores the
incoming thread registers, rewrites the interrupt return frame, and loads that
thread's CR3. The `/bin/uyield.elf` proof yields from the child back to the
shell, then resumes the child on a second yield and exits with code `42`.
The `/bin/ubusy.elf` proof never calls `Yield`; `upreemptdemo` spawns and waits
for the child without manually enabling preemption. The kernel scheduler auto-arms the
user-preemption gate when the child becomes a competing schedulable user thread,
then disables the gate after the Local APIC timer interrupt switches into the
child so it can finish and exit with code `33`.

## Current Boundary

The init process is loaded, a launch context is built, and the kernel enters it
at CPL3 as the final boot phase. The process PML4 contains user ELF/stack
mappings plus supervisor-only kernel runtime mappings needed for syscall
handling. The shell command dispatcher is observable in the serial boot log.

The remaining boundary is general scheduling: normal external commands,
pipelines, foreground jobs, and the shell `run` command use the user scheduler.
The older bounded `RunProcess` interrupt path has been removed, so child
execution no longer has a private parent-frame return path outside the scheduler.
Cooperative user `Yield`, kernel-blocking `Wait`, and the Local APIC timer path
can save and switch full CPL3 frames between separate processes, and child
`Exit` writes the exit status into the waiter address space, resumes the blocked
`Wait` syscall with success, then schedules another runnable user process. The
timer gate is now automatically armed when kernel process-control syscalls make
more than one user thread runnable/running, but it is still not a continuously
running global user scheduler. Interactive stdin is key-at-a-time and nonblocking. The
framebuffer terminal has kernel-owned text scrollback, but it is still a simple
local console rather than a full terminal emulator with escape sequences, PTYs,
or independent per-process sessions. Kernel pipe objects are currently bounded
in-memory buffers; pipeline stages now run concurrently under Local APIC
user preemption maintained from scheduler state, and empty/full pipe operations can block the current
user thread until peer activity wakes it. `SleepTicks` blocks the current user
thread and resumes it from the Local APIC timer interrupt path once its wake
tick is reached. Stdio file redirection targets VFS files only. A full terminal
line discipline, dynamically sized streaming pipes, and general always-on
preemptive user-task scheduling are still future work.
Environment storage is process-local and fixed-capacity, not a dynamically
allocated `envp` stack yet.
