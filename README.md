# IanOS

IanOS is a from-scratch x86_64 UEFI operating-system project. It uses the
Mattas kernel, a repo-owned EFI bootloader, loads an ELF64 kernel directly, exits UEFI boot
services, and enters a freestanding C++ kernel.

The current milestone is **Kernel Runtime Core**, a bootable runtime foundation:

- `BOOTX64.EFI` custom UEFI application
- ELF64 kernel loader
- custom UEFI boot manager menu with normal/recovery choices and an explicit disabled legacy-BIOS entry
- boot handoff ABI with memory map, GOP framebuffer, and ACPI RSDP
- kernel physical range and entry-point handoff
- framebuffer + COM1 serial logging with INFO/WARN/ERROR/DEBUG levels
- panic/assert helpers
- GDT and IDT setup with detailed fatal exception diagnostics
- ring-3 GDT selectors, loaded TSS, and `iretq` transition into the init process
- TSS IST1 double-fault stack
- page-fault diagnostics with CR2, RIP, RSP, RFLAGS, and decoded error bits
- bitmap physical-memory allocator with range reservation and contiguous allocation
- x86_64 4-level VMM map/unmap/translate helpers plus local page invalidation, duplicate/unaligned/absent-map rejection diagnostics, map/unmap counters, and post-SMP remote TLB shootdown requests outside active LAPIC-timer preemption windows
- PMM-backed high virtual kernel heap with physical allocation/free/failure/invalid-free diagnostics and peak-used tracking
- user address-space PML4 creation with user ELF/stack mappings and supervisor-only kernel runtime mappings
- ACPI RSDP/RSDT/XSDT/MADT parser plus MCFG/ECAM range discovery with boot-time table-scan, checksum, malformed-entry, and ECAM validation diagnostics
- Local APIC MMIO mapping, per-CPU software enable, register helpers, masked timer countdown probing, EOI support, fixed IPI delivery, interrupt/vector accounting exposed through `/proc/irq/summary` and Linux-like `/proc/interrupts`, and unmasked periodic timer interrupt dispatch for kernel scheduling
- MADT-driven SMP startup with a repo-generated AP trampoline, INIT/SIPI delivery, AP long-mode check-in, AP-side descriptor/timer setup, parked AP state exposed through `/proc/cpu/topology`, and a fixed-depth AP work queue driven by fixed IPIs, including an AP-side TLB invalidate command
- bounded PCI discovery across all 256 buses with ACPI MCFG/ECAM-preferred config access, legacy CF8/CFC fallback, ECAM/legacy cross-checking, read-only config-space probe/fuzz counters, malformed config-access rejection, config-path read/write counters, command-enable success accounting, multifunction handling, class counters, BAR resource discovery, command/status helpers, driver candidate matching, metadata-only PCI driver bindings, `/proc/pci/summary` diagnostics, and `/proc/pci/devices` retained-device rows
- scheduler thread records with IDs, states, affinity, and mapped kernel stacks
- x86_64 context switch assembly for cooperative and interrupt-return switching
- PIT timer programming remains available as a fallback path
- Local APIC timer-driven preemptive switching demo between two runnable kernel threads
- syscall dispatcher and a working `int 0x80` DPL3 gate used by CPL3 init
- syscall ABI entries for scheduler/process introspection, scheduler runtime stats, process/thread record enumeration with user-thread block reasons and wait targets, fixed startup 64-byte argument metadata, 80-byte environment value metadata with environment mutation, current process/cwd metadata, RTC date/time export, user launch-context export, PMM memory stats, framebuffer info, device inventory counts and detailed device records, block-device stats and sector reads, mount-table enumeration, keyboard `ReadKey`, process-created `/dev/tty` stdin/stdout/stderr descriptors with fd-backed raw/canonical terminal I/O, VFS node enumeration, direct directory-entry reads, generated virtual `/proc` files including process, mount, filesystem, command-line, self-status, current fd-table, and per-process `/proc/<pid>` snapshots, `/proc/self/fd/N` readlink resolution, character-device file descriptors, path-based metadata, process reaping, raw VFS handles, and process-local file descriptor open/read/seek/close operations with cwd-relative path resolution
- syscall ABI support for spawning runnable user process records from VFS-backed ELF64 images with bounded argv metadata, user-scheduler next-thread selection, scheduler-backed child waits, plus parent-scoped SIGTERM/SIGKILL-like kill/wait/reap lifecycle control and termination-reason reporting
- freestanding `init.elf` plus `/bin/*.elf` userland command artifacts with shared syscall ABI wrappers, boot-scripted verification, and interactive shell command dispatch
- UEFI handoff of boot-module user images and kernel-side ELF64 user process loader
- fixed-capacity userspace process and user-thread metadata with PML4, image, stack, file descriptors, owned pages, and lifecycle records
- safe boot self-tests for BootInfo, PMM allocation/failure accounting, VMM map/range/rejection accounting, heap, ACPI table/ECAM diagnostics, and scheduler creation

## Build

```powershell
powershell -ExecutionPolicy Bypass -File scripts/check-prereqs.ps1
powershell -ExecutionPolicy Bypass -File scripts/build.ps1
```

The final bootable FAT16 disk image is written to
`build/out/image/kernel.img`. Build intermediates such as `BOOTX64.EFI`,
`kernel.elf`, `init.elf`, user command ELFs, and the manifest remain under
`build/out` for inspection. The editable ESP staging directory used by QEMU is
assembled at `build/esp`. `scripts/build.ps1` and `scripts/run.ps1` also print
an image report with the fixed disk-image byte size, FAT capacity, estimated
allocated payload bytes, free estimate, file counts, and key artifact sizes so
kernel and userland growth is visible even when `kernel.img` capacity stays the
same.

## Run

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1
```

or:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run.ps1
```

The launcher uses QEMU + OVMF and boots the default UEFI removable-media path:

```text
build/esp/EFI/BOOT/BOOTX64.EFI
build/esp/kernel.elf
build/esp/user/init.elf
build/esp/bin/hello.elf
build/esp/bin/args.elf
build/esp/bin/cat.elf
build/esp/bin/ls.elf
build/esp/bin/blk.elf
build/esp/bin/lsblk.elf
```

Normal runs show the custom IanOS UEFI boot manager first. Choose
`Normal boot (UEFI)` and press Enter to reach the interactive shell at the
`ianos> ` prompt. Choose `Recovery shell (UEFI)` to enter a separate rescue
target with a `recovery# ` prompt, startup diagnostics, and repair-focused
commands such as `status`, `check`, `logs`, `mounts`, `files`, `processes`,
and `hardware`; type `shell` there only when you want to continue into the
normal command environment. Choose `Debug boot (UEFI)` when you want the full kernel log stream
painted to the framebuffer during boot; normal mode keeps those logs in serial
and the kernel log buffer so the visible QEMU boot stays clean. The
`BIOS/legacy boot` entry is visible but intentionally disabled until a separate
BIOS boot path exists. Normal shell entry prints a compact fastfetch-style
system summary with IanOS and Mattas kernel details before the prompt. Type
commands in that window, for example `help`, `ls`, `export EDITOR=hksh`,
`env`, `printenv PATH`, `which grep`, `false ; echo $?`, `err 2> /tmp/err.txt`, `hello test`,
`args one two`, `loadavg`, `scheddebug`, `buddyinfo`, `heapinfo`, `procvmstat`, `procstat`, `pmap`, `version`, `limits`, `imginfo`, `abi`, `fastfetch`, `sysctl -a`, `nproc`, `lscpu`, `cpuinfo`, `schedstat`, `vmstat`, `top`, `pstree`, `/bin/processes.elf`, `groups`, `pidof init`, `lsattr /etc/os-release`, `namei /mnt/boot/bin/hello.elf`, `tree /mnt/boot`, `statfs /mnt/boot/bin/hello.elf`, `/bin/filesystems.elf`, `find /proc/sys`, `hexdump /disk/bootsector.bin`, `readelf /bin/readelf.elf`, `file /bin/hello.elf /etc/os-release /proc/version`, `sha256sum /etc/os-release`, `sha224sum /etc/hostname`, `sha512sum /etc/hostname`, `sha384sum /etc/hostname`, `sha1sum /etc/hostname`, `md5sum /etc/hostname`, `cksum /etc/hostname`, `fold -w 12 /proc/version`, `head -n 2 /etc/os-release`, `tail -n 2 /etc/os-release`, `printf '%s:%d:%x\n' IanOS 42 255`, `dd if=/etc/hostname of=/tmp/dd-host bs=6 count=1`, `wc /etc/hostname /proc/sys/kernel/hostname`, `printf 'alpha beta\n' | xargs echo prefix`, `yes IanOS count=3`, `od -t x1 /etc/hostname`, `base64 /etc/hostname`, `base64 -d /tmp/host64`, `/bin/which.elf grep`, `cal 7 2026`, `realpath ../user/./init.elf`, `cat /etc/hostname /proc/sys/kernel/hostname`, `cmp /etc/hostname /proc/sys/kernel/hostname`, `strings /proc/version`, `nl /etc/os-release`, `tr IOS ios /etc/os-release`, `sed s/IanOS/IanOS-dev/ /etc/os-release`, `cut -d = -f 2 /etc/os-release`, `paste /etc/hostname /proc/sys/kernel/hostname`, `rev /etc/hostname`, `tac /etc/os-release`, `uniq -c /tmp/words`, `uniq -d /tmp/words`, `uniq -u /tmp/words`, `seq 3`, `expr 7 + 5`, `date`, `rtc`, `/bin/ttystat.elf`, `/bin/ethtool.elf`, or `/bin/cat.elf /bin/args.elf`. Input is echoed; Backspace,
arrow keys, history navigation, and tab completion work in the current line.
Close the QEMU window to end the run. Use
`run.ps1 -Headless` or `scripts/run.ps1 -Headless` for timed serial-only QEMU
runs, and `scripts/run.ps1 -Recovery` for a direct recovery boot without using
the menu. The long
boot-scripted command transcript is reserved for `scripts/verify.ps1`. Normal
interactive shell output is prefixless; `[shell]` tags are used only in the
verifier transcript.

## Verify

```powershell
powershell -ExecutionPolicy Bypass -File scripts/verify.ps1
```

The verification script checks prerequisites, builds artifacts, boots QEMU/OVMF
headlessly, validates `build/qemu-serial.log` for required runtime/scheduler
markers, rejects fatal exception markers, checks user ELF artifacts for
unresolved freestanding runtime symbols, validates `build/out/image/kernel.img`, and
confirms QEMU cleanup.

See [docs/verification.md](docs/verification.md) for the full gate list.

## Status

Implemented:

- Custom UEFI bootloader, direct ELF64 loading, `ExitBootServices()` handoff.
- Bootloader-owned text UI with arrow/number-key selection for normal UEFI boot
  and UEFI recovery shell mode.
- Versioned packed BootInfo ABI and validation.
- Serial/framebuffer diagnostics and panic path.
- PMM, VMM, heap, ACPI MADT and MCFG discovery, Local APIC timer groundwork.
- MADT-driven CPU topology exposed through `/proc/cpu/summary` and `/proc/cpu/topology`, AP trampoline startup, AP-side GDT/TSS/IDT loading, parked AP state, repeated fixed-IPI AP work-queue completion, a boot-proven AP-side TLB invalidate command, and VMM-triggered remote shootdown requests after AP startup during non-preemptive kernel phases.
- Scheduler data model and kernel-thread creation.
- Local APIC timer interrupt dispatch and a verified preemptive kernel-thread switch.
- TSS/GDT groundwork and verified ring-3 transition.
- Address-space creation, syscall dispatcher, and process metadata stubs.
- Freestanding userland `init.elf` build, bootloader handoff, and kernel ELF mapping into a process address space.
- Userspace process lifecycle operations for created, runnable, and exited states.
- Userspace exit-code storage, live-process accounting, and reusable metadata slots for reaped processes.
- Fixed per-process startup argument and environment storage with syscall enumeration plus environment set/unset for init-style `argv` and `envp`.
- Explicit current user execution-context save/restore used by child process dispatch and return.
- Process-local file descriptor tables backed by VFS handles, including cwd-relative path resolution and seekable read cursors.
- User-thread records tied to loaded processes, including TID, entry, stack pointer, PML4, and runnable state.
- Local APIC user preemption is owned by the kernel user scheduler, which auto-arms the gate when more than one runnable/running user thread exists and drops it when contention disappears.
- Shared userspace ABI structures and syscalls for indexed process and user-thread snapshots.
- Current process ID plus per-process current-directory get/set syscalls validated against VFS directories, used by process-local relative opens.
- Compact current identity syscall and `ids` shell command exposing user PID/TID, parent PID, kernel scheduler thread ID, and current CPU ID.
- Shared userspace ABI structure and syscall for launch-context export.
- Shared userspace ABI structure and syscall for scheduler runtime statistics.
- Shared userspace ABI structures and syscalls for indexed VFS node enumeration, direct directory-entry enumeration, plus path-based `stat` metadata including type, size, link count, writable/readable flags, and normalized path.
- Boot-scripted and interactive userland shell running from CPL3 `init.elf` with fd-backed terminal I/O, `help`, `clear`, `history`, `exit`, `echo`, `status`, `pid`, `ids`, `fgpgid`, `argv`, `env`, `export`, `unset`, `which`, `stat`, `counts`, `spawn`, `jobs`, `fg`, `bg`, `stop`, `usched`, `nextuser`, `run`, `kill`, `wait`, `reap`, `pwd`, `cd`, `ls`, `cat`, `sh`, `ps`, `mem`, `devices`, `fb`, and `ticks` built-ins, plus `PATH`-resolved `/bin/*.elf` external command execution with stdin, stdout, and stderr redirection. `/bin/sh.elf` runs scripts and `-c` command lines quietly while sharing the same redirection and pipeline machinery. Userland includes RTC-backed `date`, CMOS RTC diagnostics through `/bin/rtc.elf`, `/etc/hostname` plus external `hostname`, Linux-like external `id`, external `groups`, external `fastfetch`, CPU-count reporting through `/bin/nproc.elf`, runtime `/proc/cpuinfo` reporting through `/bin/cpuinfo.elf`, Linux-like scheduler/process counter streaming through `/bin/procstat.elf`, process address-space mapping through `/bin/pmap.elf`, kernel sysctl reads through `/bin/sysctl.elf`, mount-table `/bin/mount.elf`, filesystem-usage `/bin/df.elf`, subtree-usage `/bin/du.elf`, filesystem type inventory through `/bin/filesystems.elf`, VFS mutation counters through `/bin/vfsstat.elf`, `/proc/meminfo` streaming through `/bin/meminfo.elf`, block-device inventory through `/bin/lsblk.elf`, boot handoff inspection through `/bin/bootinfo.elf`, Linux-shaped disk counters through `/bin/diskstats.elf`, partition inventory through `/bin/partitions.elf`, descriptor-table inspection through `/bin/lsof.elf`, framebuffer geometry reporting through `/bin/fbset.elf`, PCI device inventory through `/bin/lspci.elf`, class-based device inventory through `/bin/lsdev.elf`, Linux-shaped major device inventory through `/bin/devices.elf`, interrupt summary accounting through `/bin/irqstat.elf`, Linux-like interrupt table streaming through `/bin/interrupts.elf`, memory-manager accounting through `/bin/mmstat.elf`, PMM free-run reporting through `/bin/buddyinfo.elf`, kernel heap diagnostics through `/bin/heapinfo.elf`, network interface diagnostics through `/bin/netstat.elf`, Linux-shaped route-table diagnostics through `/bin/route.elf`, `ip link`/`ip addr`/`ip route` reporting through `/bin/ip.elf`, ifconfig-style link counters through `/bin/ifconfig.elf`, ethtool-style e1000 link, duplex, register, and ring diagnostics through `/bin/ethtool.elf`, driver-manager summaries and bound-device rows through `/bin/lsdrv.elf`, boot-module inventory through `/bin/lsmod.elf`, VFS-backed kernel-log streaming through `/bin/kmsg.elf`, runnable/live load reporting through `/bin/loadavg.elf`, scheduler debug streaming through `/bin/scheddebug.elf`, live pipe-table diagnostics through `/bin/pipeinfo.elf`, PID-based signal-style process termination through `/bin/kill.elf`, process lookup through `/bin/pgrep.elf`, PID-list lookup through `/bin/pidof.elf`, and name-based termination through `/bin/killall.elf`. `ps` surfaces process groups, user-thread state, block reason, syscall/run-tick/switch/preempt accounting, and pipe/process wait targets.
- Spawn/usched/nextuser/run/wait/reap shell commands that create a start-suspended process from `/bin/hello.elf alpha beta`, persist child argv metadata, inspect user-thread scheduling state before it is runnable, start it with `run`, observe the exit code, and clean it up; background `loop &`, `%+` job references, `stop %+`, `bg %+`, `sleep 5 &`/`fg %+`, and `loop &` plus `sleep 5 &`/`wait -n` prove tracked jobs can run outside the prompt, be stopped, resumed, controlled by shell job IDs, foregrounded through scheduler-backed `Wait`, and collected through scheduler-backed `WaitAny`; foreground and background pipelines are assigned one process group and waited/reaped as multi-process jobs; direct `which grep`, `false ; echo $?`, `$?`/`$VAR`/`${VAR}` expansion in init shell and `/bin/sh.elf`, quote-aware argv through single quotes, double quotes, and escaped spaces, `false && ... || ...`, `test -d /tmp && ...`, `err 2> /tmp/stderr.txt`, `hello gamma delta`, `args one two`, `/bin/cat.elf /bin/args.elf`, multi-file `/bin/cat.elf /etc/hostname /proc/sys/kernel/hostname`, multi-file `/bin/wc.elf /etc/hostname /proc/sys/kernel/hostname`, `head -n 2 /etc/os-release`, `tail -n 2 /etc/os-release`, `/bin/env.elf` after `export EDITOR=hksh`, `/bin/printenv.elf`, `/bin/fastfetch.elf`, `/bin/sysctl.elf`, `/bin/groups.elf`, `/bin/nproc.elf`, `/bin/find.elf`, `/bin/hexdump.elf`, `/bin/readelf.elf`, `/bin/file.elf`, `/bin/lsattr.elf`, `/bin/namei.elf`, `/bin/tree.elf`, `/bin/statfs.elf`, `/bin/filesystems.elf`, `/bin/vfsstat.elf`, `/bin/sha256sum.elf`, `/bin/sha224sum.elf`, `/bin/sha512sum.elf`, `/bin/sha384sum.elf`, `/bin/sha1sum.elf`, `/bin/md5sum.elf`, `/bin/cksum.elf`, `/bin/fold.elf`, `/bin/printf.elf`, `/bin/dd.elf`, `/bin/xargs.elf`, `/bin/yes.elf`, `/bin/od.elf`, `/bin/which.elf`, `/bin/cal.elf`, `/bin/realpath.elf`, `/bin/cmp.elf`, `/bin/strings.elf`, `/bin/nl.elf`, `/bin/tr.elf`, `/bin/sed.elf`, `/bin/cut.elf`, `/bin/paste.elf`, `/bin/rev.elf`, `/bin/tac.elf`, `/bin/seq.elf`, `/bin/expr.elf`, `/bin/stat.elf`, `whoami`, `hostname`, `/bin/id.elf`, `basename`, `dirname`, `head`, `tail`, `sort`, `uniq`, `sh /tmp/script`, `/bin/ctx.elf`, `cat | tee | wc`, `echo | lsof | grep`, `/bin/fdinh.elf`, `/bin/ln.elf`, `/bin/readlink.elf`, `/bin/truncate.elf`, `/bin/blk.elf`, `/bin/mount.elf`, `/bin/df.elf`, `/bin/du.elf`, `/bin/lsblk.elf`, `/bin/findmnt.elf`, `/bin/iostat.elf`, `/bin/diskstats.elf`, `/bin/partitions.elf`, `/bin/lsmem.elf`, `/bin/iomem.elf`, `/bin/bootinfo.elf`, `/bin/fbset.elf`, `/bin/lspci.elf`, `/bin/lsdev.elf`, `/bin/devices.elf`, `/bin/irqstat.elf`, `/bin/interrupts.elf`, `/bin/mmstat.elf`, `/bin/buddyinfo.elf`, `/bin/heapinfo.elf`, `/bin/procvmstat.elf`, `/bin/procstat.elf`, `/bin/netstat.elf`, `/bin/route.elf`, `/bin/ip.elf`, `/bin/ifconfig.elf`, `/bin/ethtool.elf`, `/bin/lsdrv.elf`, `/bin/lsmod.elf`, `/bin/kmsg.elf`, `/bin/loadavg.elf`, `/bin/processes.elf`, `/bin/cmdline.elf`, `/bin/procstat.elf`, `/bin/pipeinfo.elf`, `/bin/pmap.elf`, `/bin/version.elf`, `/bin/limits.elf`, `/bin/imginfo.elf`, `/bin/abi.elf`, `/bin/kill.elf`, `/bin/pgrep.elf`, `/bin/pidof.elf`, `/bin/lscpu.elf`, `/bin/cpuinfo.elf`, `/bin/schedstat.elf`, `/bin/vmstat.elf`, `/bin/top.elf`, `/bin/pstree.elf`, `/bin/killall.elf`, and `/bin/ls.elf` command execution proves the same lifecycle behind normal shell command dispatch for multiple external programs, inherited environment metadata, selected environment lookup, status-aware chaining, file-descriptor reads, stderr capture, script lines, direct VFS directory enumeration, current-context reporting, inherited stdio across pipeline processes, shared inherited VFS stdio offsets, `/proc/self/fd/N` readlink and realpath resolution, RAM-backed hard links and truncation, direct block-device syscall diagnostics, bounded hex/ASCII file reads, ELF64 header inspection, file-type classification, VFS attribute reporting, path-component resolution, recursive directory traversal, filesystem ownership reporting, VFS mutation-counter reporting, SHA-256, SHA-224, SHA-512, SHA-384, SHA-1, MD5, and CRC32 content hashing, bounded line wrapping, formatted script output, calendar rendering from date arguments, block-sized file copying, xargs-driven child spawning from stdin, repeated text generation, octal/hex byte dumping, PATH-backed command lookup, dual-descriptor file comparison, printable string extraction, numbered, translated, substituted, field-selected, line-pasted, reversed text output, numeric sequence generation, integer expression evaluation, CPU-count, CPU-topology, CPU-info, scheduler-stat, VM-stat, and top-style process/thread reporting, process-tree reporting, boot command-line streaming, PID-list lookup, userspace mount-table inspection, mount filtering, block I/O statistics, partition inventory, physical-memory range reporting, framebuffer geometry reporting, PCI device inventory reporting, Linux-shaped device major reporting, interrupt accounting, direct interrupt table streaming, memory-manager accounting, network interface diagnostics, boot-module inventory, VFS-backed kernel-log streaming, filesystem usage reporting, kernel pipe-table visibility, process address-space map reporting, process-name lookup, and userspace-driven signal-style process termination.
- `/bin/uniq.elf` suppresses adjacent duplicate lines by default and supports `-c` count-prefix, `-d` duplicate-only, and `-u` unique-only output for each emitted run. `/bin/base64.elf` supports descriptor-backed encode and whitespace-tolerant `-d` decode mode.
- Validated and executed user launch context with RIP, RSP, CR3, CS, SS, and RFLAGS for `iretq` entry.
- User image/stack physical-page ownership tracking with data-page release and user-thread slot reclamation on process reap.
- PCI AHCI/e1000/VGA candidate binding records with required command-bit metadata, plus driver-manager lifecycle/import diagnostics exposed through `/proc/driver/summary` and bound-device rows exposed through `/proc/driver/devices`, without enabling devices before real drivers exist.
- AHCI PCI probe metadata with controller BDF, ABAR base/size, command-bit requirements, HBA CAP/GHC/version reads, implemented-port bitmap parsing, active-port status sampling, read-only ATA IDENTIFY command completion with LBA28/LBA48 sector-count retention, reusable bounded sector reads, LBA0 READ DMA EXT validation, a fixed-size block cache with hit/miss/fill/eviction/invalid-read cause/backend-failure/occupancy statistics exposed through `/proc/block/bootdisk`, Linux-shaped boot-disk counters exposed through `/proc/diskstats`, Linux-shaped partition inventory exposed through `/proc/partitions`, bounded multi-sector reads, and a recursive read-only FAT16 mount under `/mnt/boot`.
- e1000 PCI probe metadata with adapter BDF, MMIO/IO resources, command-bit requirements, MMIO register sampling, decoded link-state diagnostics, interrupt mask/ack diagnostics, MAC address metadata, PCI command enablement, RX/TX descriptor-ring programming with register readback validation, reusable TX buffer-pool submission with boundary rejection counters, reusable RX polling, RX ownership checks, a Linux-shaped `/proc/net/route` view keyed from adapter presence/link state, and a bounded descriptor-completion TX smoke.
- VGA PCI probe metadata with adapter BDF, MMIO resource, and command-bit requirements.
- Unified device inventory for probed storage, network, and display resources.
- GOP framebuffer console with repaintable scrollback, viewport-follow state, render/glyph/cursor/scroll/reset-line counters exposed through `/proc/tty/summary`, and boot-tested viewport plus input-line repaint invariants.
- PS/2 keyboard IRQ1 translation feeding the terminal input discipline, diagnostic raw `ReadKey`, fd `0` reads for shell stdin, fd `1`/`2` terminal writes for stdout/stderr, terminal-control raw/canonical input modes, scrollback movement, prompt-line reset/repaint support, kernel-side terminal input/write/scroll counters exposed through `/proc/tty/summary`, queue high-water tracking, and overflow/drop accounting.
- Ctrl-letter keyboard translation plus shell Ctrl+C/Ctrl+Z polling while waiting on foreground work. The shell now maintains a kernel-visible terminal foreground process group, exposes it through `fgpgid`, foregrounds child groups around `exec`, `fg`, and pipelines, restores its own PGID afterward, delivers Ctrl+C as SIGTERM through `KillProcessGroup`, and delivers Ctrl+Z through scheduler-visible stopped process-group state that can be resumed with `bg` or `fg`.
- Shared userspace ABI structure for indexed device inventory records.
- Class-filtered device inventory lookup for storage, network, and display records.
- Shared userspace ABI structures and syscalls for PMM memory statistics, boot-block-device stats and LBA sector reads, and mounted filesystem records.
- Early fixed-capacity VFS namespace with read-only memory-backed `/boot/kernel.elf`, `/user/init.elf`, and `/bin/hello.elf` files plus reusable open-file handles.
- Boot self-tests, including generated `/proc/meminfo`, `/proc/iomem`, `/proc/rtc`, `/proc/buddyinfo`, `/proc/heapinfo`, `/proc/vmstat`, `/proc/uptime`, `/proc/loadavg`, `/proc/sched_debug`, `/proc/stat` scheduler and userspace block/wake/preemption diagnostics, `/proc/block/bootdisk` block-cache diagnostics, `/proc/diskstats` boot-disk counter diagnostics, `/proc/partitions` partition-table diagnostics, `/proc/driver/summary` driver-manager diagnostics, `/proc/driver/devices` bound-driver records, `/proc/pci/summary` PCI scan diagnostics, `/proc/pci/devices` PCI device rows, `/proc/irq/summary` IRQ/APIC diagnostics, `/proc/interrupts` vector table diagnostics, `/proc/tty/summary` TTY/framebuffer diagnostics, `/proc/cpuinfo`, `/proc/cpu/summary`, and `/proc/cpu/topology` CPU/SMP diagnostics, `/proc/net/route`, `/proc/bootinfo`, `/proc/processes`, `/proc/mounts`, `/proc/filesystems`, `/proc/fs/vfs` RAM-filesystem mutation diagnostics, `/proc/cmdline`, `/proc/sys/kernel/hostname`, `/proc/sys/kernel/ostype`, `/proc/sys/kernel/osrelease`, `/proc/sys/kernel/version`, `/proc/self/status`, `/proc/self/fd`, `/proc/1/status`, and `/proc/1/fd` virtual-file reads.

Partial:

- Local APIC is enabled and readable on started CPUs, including masked timer LVT configuration/readback, countdown probing, fixed IPI delivery to a parked AP on vector `0x41`, AP-side TLB invalidate command handling, unmasked periodic timer interrupts on vector `0x40`, timer tick and vector-dispatch accounting, EOI accounting, a boot-proven preemptive switch between two runnable kernel threads, and APIC-backed blocking `SleepTicks` wakeups for CPL3 user threads. IOAPIC entries are masked during setup, and ACPI-aware masked redirection entries can be programmed, read back, and counted.
- Driver manager imports PCI AHCI/e1000/VGA binding records into driver-owned device records with command-bit requirements. Devices remain bound but not started until real hardware drivers are implemented.
- AHCI probing maps the controller ABAR, samples HBA global/port metadata, allocates command/FIS/table buffers, completes an ATA IDENTIFY command, reads LBA0 from the boot disk to validate the FAT boot signature, exposes that sector as `/disk/bootsector.bin`, initializes a fixed-size block cache, parses the FAT16 geometry, and recursively mounts disk entries read-only under `/mnt/boot` with disk-backed VFS flags. It still does not provide writes or a dynamically sized cache yet.
- e1000 probing identifies NIC MMIO/IO metadata, enables required PCI command bits, maps a bounded MMIO window, masks and acknowledges NIC interrupts during setup, samples stable identity/control/status/MAC registers, decodes link-up/full-duplex/speed/bus-width metadata, allocates RX/TX descriptor memory with tracked RX buffers and per-descriptor TX buffers, programs the first RX/TX rings, verifies descriptor base/length register readback, exposes reusable `transmit_frame` and `poll_receive` primitives, proves TX null/length rejection boundaries, checks idle RX descriptor ownership, proves an empty receive poll, and submits one bounded 60-byte Ethernet TX smoke descriptor. A packet network stack is still future work.
- VGA probing identifies display MMIO metadata while GOP remains the active framebuffer output path.
- Device inventory normalizes probed AHCI, e1000, and VGA resources for later driver startup and user-visible device enumeration.
- VFS currently exposes boot-provided memory files, directories, direct directory entries for `ls`-style traversal, kernel-backed `/dev/null`, `/dev/zero`, `/dev/tty`, and `/dev/console` character devices, AHCI-backed `/disk/bootsector.bin`, recursive read-only FAT16 files/directories under `/mnt/boot`, userspace-visible mount records for `/` and `/mnt/boot`, path-based stat metadata with link counts, RAM-backed hard links, 4 KiB RAM-backed writable files across 32 live RAM-file slots in a 512-node namespace, RAM-backed file truncation, RAM-backed rename for writable files and empty directories, RAM-mutation success/rejection/write-clipping diagnostics exposed through `/proc/fs/vfs`, and open/read/close handles. Terminal devices write to the kernel terminal and read from a terminal-owned input discipline fed by the PS/2 keyboard driver, with raw mode for shell editing and canonical line buffering available through terminal control. Userspace resolves cwd-relative paths, while the VFS canonicalizes absolute paths containing `.`, `..`, or repeated separators before lookup and RAM-file mutations. Writable disk filesystems and a larger block-cache policy are still future work.
- Scheduler can create runnable kernel threads with stacks and switch between them cooperatively and from Local APIC timer interrupts. Online APs are currently parked after proving repeated fixed-IPI work-queue handling rather than participating in the general run queue.
- APIC timer preemption is proven by the boot demo. PIT-backed sleep/wake scaffolding remains only a fallback path; CPL3 blocking sleep/wake is now driven by the Local APIC tick, and user preemption is maintained from scheduler state rather than only from shell wait guards.
- The syscall dispatcher and user address-space mapper are tested in-kernel and at CPL3. `init.elf` is built, loaded by the bootloader, mapped into a process address space, assigned `/dev/tty` on fd 0/1/2, assigned a main user thread, converted into a validated launch context, entered with `iretq`, and proven through user `int 0x80` debug logging plus boot-scripted shell output and interactive shell readiness.
- `hello.elf`, `args.elf`, `cat.elf`, `ls.elf`, `hostname.elf`, `fastfetch.elf`, `sysctl.elf`, `find.elf`, `hexdump.elf`, `readelf.elf`, `file.elf`, `lsattr.elf`, `namei.elf`, `tree.elf`, `statfs.elf`, `meminfo.elf`, `filesystems.elf`, `vfsstat.elf`, `sha256sum.elf`, `sha224sum.elf`, `sha512sum.elf`, `sha384sum.elf`, `sha1sum.elf`, `md5sum.elf`, `cksum.elf`, `fold.elf`, `printf.elf`, `dd.elf`, `cmp.elf`, `strings.elf`, `nl.elf`, `tr.elf`, `sed.elf`, `cut.elf`, `paste.elf`, `rev.elf`, `tac.elf`, `seq.elf`, `expr.elf`, `id.elf`, `stat.elf`, `sort.elf`, `uniq.elf`, `base64.elf`, `tee.elf`, `ids.elf`, `groups.elf`, `ctx.elf`, `fds.elf`, `lsof.elf`, `fdinh.elf`, `ln.elf`, `readlink.elf`, `realpath.elf`, `truncate.elf`, `blk.elf`, `mount.elf`, `df.elf`, `du.elf`, `lsblk.elf`, `diskstats.elf`, `partitions.elf`, `fbset.elf`, `lspci.elf`, `lsdev.elf`, `devices.elf`, `irqstat.elf`, `interrupts.elf`, `mmstat.elf`, `buddyinfo.elf`, `heapinfo.elf`, `procvmstat.elf`, `netstat.elf`, `route.elf`, `ip.elf`, `ifconfig.elf`, `ethtool.elf`, `lsdrv.elf`, `lsmod.elf`, `kmsg.elf`, `loadavg.elf`, `scheddebug.elf`, `pipeinfo.elf`, `devio.elf`, `tty.elf`, `ttystat.elf`, `stty.elf`, `ttyread.elf`, `clear.elf`, `kill.elf`, `pgrep.elf`, `pidof.elf`, `nproc.elf`, `lscpu.elf`, `cpuinfo.elf`, `rtc.elf`, `schedstat.elf`, `vmstat.elf`, `top.elf`, `pstree.elf`, `processes.elf`, `cmdline.elf`, `procstat.elf`, and `killall.elf` are built, loaded by the bootloader as boot modules, exposed through VFS, spawnable into runnable process records with argv metadata and process-group identity, executable through the scheduler-backed wait path, able to read arguments, files, VFS metadata, inherited stdin/stdout, current process/thread identity, current user context, descriptor-table snapshots, syscall accounting, boot block-device metadata, mount records, filesystem usage, VFS mutation-counter reporting, partition inventory, framebuffer geometry, PCI device inventory, interrupt accounting, direct interrupt table streaming, memory-manager accounting, PMM free-run availability, network interface diagnostics, boot-module inventory, VFS-backed kernel-log streaming, runnable/live load reporting, scheduler debug streaming, pipe-table snapshots, character-device I/O, terminal input-mode control, terminal clearing, terminal/framebuffer statistics streaming, canonical terminal stdin reads, fd-link resolution, RAM-backed hard-link and truncate syscalls, bounded hex/ASCII file output, ELF64 header inspection, file-type classification, VFS attribute reporting, path-component resolution, recursive directory traversal, filesystem ownership reporting, VFS mutation-counter reporting, SHA-256, SHA-224, SHA-512, SHA-384, SHA-1, MD5, and CRC32 content hashing, Base64 encoding/decoding, bounded line wrapping, formatted script output, block-sized file copying, stdin-driven child spawning, dual-file comparison, printable string extraction, numbered, translated, substituted, field-selected, line-pasted, reversed text output, reverse line-order output, numeric sequence generation, integer expression evaluation, CPU-count, CPU-topology, CPU-info, RTC diagnostics, scheduler-stat, VM-stat, and top-style process/thread reporting, process-tree reporting, boot command-line streaming, PID-list lookup, and process-control syscalls, and able to return to the shell through `Exit`.
- Process-local file descriptors are attached to the current user process derived from the selected user thread, resolve relative paths against that process cwd, track VFS paths and offsets, and make spawned children inherit stdin/stdout/stderr by reopening VFS descriptors or copying pipe endpoints. Compatibility accessors still expose the older active-process terminology while syscall dispatch uses the explicit current-context boundary.
- Reaping returns tracked user image and stack data pages plus owned user-half page-table hierarchies to the PMM.
- ACPI parser covers platform discovery tables needed for APIC/SMP and PCIe ECAM groundwork, not the full ACPI namespace.

Future:

- Thread lifecycle reaping and stack reclamation.
- Broaden process teardown coverage as more resource types are added.
- IO APIC routing and interrupt controller policy.
- AP scheduler participation, nested interrupt accounting, active-LAPIC-safe shootdown handling, user address-space shootdown integration, and cross-CPU scheduler nudges.
- PCIe ECAM/MMCONFIG as the primary config path, device enablement, storage-backed filesystems, networking.
- Userspace ABI, processes, and syscall entry.
- Per-running-thread process context for multiple user processes.
- Scheduler integration for multiple runnable CPL3 processes and child process return.
- Full terminal line discipline, dynamically sized pipe buffers, fuller POSIX-style pipe status reporting, and richer multi-line editing/history search for the interactive shell.

## Milestones

1. Kernel Runtime Core: current milestone.
2. Timer + Preemptive Kernel Threads: current milestone.
3. Scheduler lifecycle hardening and APIC-backed blocking sleep/wake.
4. SMP scheduler participation.
5. PCI/Storage.
6. Userspace.
