# Kernel Architecture

The kernel is structured as a hybrid kernel: fast core code in one address space
with explicit service boundaries for CPU, memory, interrupts, scheduling,
drivers, and future userspace.

Kernel Runtime Core modules:

- `cpu`: per-CPU GDT/TSS records, double-fault IST stacks, CPU feature discovery, MADT-derived CPU topology records with bootstrap/online/startup-attempt state exposed through `/proc/cpu/topology`, and per-CPU runtime records for scheduler, AP bootstrap work, queued AP IPI work, and parked AP state.
- `interrupts`: exception dispatch, APIC-ID-indexed interrupt scratch state, TSS IST groundwork for dedicated interrupt stacks, page-fault decoding, and halt-on-fault diagnostics.
- `mm`: bitmap physical allocator with allocation/free/failure/invalid-free diagnostics and peak-used tracking, 4-level page-table mapping helpers with map/range/unmap/rejection counters, local page invalidation, remote TLB shootdown requests for kernel VMM map/unmap changes after AP startup during non-preemptive kernel phases, address-space creation, page permission inspection, and kernel heap.
- `acpi`: RSDP/RSDT/XSDT validation, MADT platform discovery, MCFG ECAM range discovery, table-scan/checksum/malformed-entry diagnostics, and overlap-checked ECAM validation counters.
- `apic`: Local APIC MMIO mapping, per-CPU software enable, masked Local APIC timer setup/readback, countdown probing with recorded decrement metadata, unmasked periodic timer interrupt dispatch, interrupt/vector accounting exposed through `/proc/irq/summary` and Linux-like `/proc/interrupts`, ICR INIT/SIPI/fixed-IPI delivery primitives, IOAPIC discovery/register reads, mask-all setup, ACPI-aware masked redirection programming/readback, register access, enable, and EOI helpers.
- `smp`: MADT-driven bootstrap processor inventory, a generated low-memory AP trampoline, INIT/SIPI startup, long-mode AP check-in, idempotent AP-side GDT/TSS/IDT loading, AP-side masked Local APIC timer setup, a controlled AP bootstrap work item, fixed-IPI dispatch to a parked AP, a small fixed-depth per-CPU AP work queue drained by the IPI handler, AP-side TLB invalidate command handling, a high-level AP park entry, CPU topology online marking for started non-bootstrap CPUs, and aggregate startup/work counters exposed through `/proc/cpu/summary`.
- `pci`: bounded PCI discovery across all 256 buses with ACPI MCFG/ECAM-preferred config access, legacy CF8/CFC fallback, ECAM read cross-checking, config-path read/write counters exposed through `/proc/pci/summary`, command-enable success accounting, read-only config-space probe/fuzz counters, malformed config-access rejection, multifunction handling, retained device records exposed through `/proc/pci/devices`, class counters, BAR resource probing, command/status helpers, driver candidate matching, and metadata-only driver binding records.
- `sched`: kernel-thread records with IDs, states, affinity, mapped stacks, cooperative yield, and IRQ-return switching.
- `timer`: PIT fallback setup plus Local APIC timer setup for kernel scheduler ticks and the userland system tick.
- `time`: CMOS RTC snapshot reads with BCD/12-hour conversion and validation for the user-visible date/time ABI.
- `drivers`: fixed-capacity driver registry, start lifecycle, PCI-bound driver-device records imported from the PCI subsystem and exposed through `/proc/driver/devices`, lifecycle/import counters exposed through `/proc/driver/summary`, per-kind binding totals, bus-master requirement accounting, AHCI controller metadata, safe HBA register/port probing, ATA IDENTIFY, reusable READ DMA EXT sector reads on the first active SATA port, e1000 adapter metadata probing with bounded MMIO register sampling, decoded link-state diagnostics, interrupt masking/acking, RX/TX ring programming with descriptor register readback validation, reusable TX buffer-pool submission with null/length rejection diagnostics, reusable RX polling, RX idle ownership checks, and TX descriptor completion smoke, VGA adapter metadata probing, and unified device inventory.
- `block`: boot-disk abstraction over AHCI sector reads with a small fixed-size read-through sector cache, bounded multi-sector reads, cache hit/miss/fill/eviction/invalid-read cause/backend-failure/occupancy statistics exposed through `/proc/block/bootdisk`, largest-request tracking, and last-LBA tracking.
- `fs`: 256-node virtual filesystem namespace with VFS-owned `.`, `..`, and repeated-separator normalization for absolute paths, directories, direct child directory-entry enumeration, kernel-backed `/dev/null`, `/dev/zero`, `/dev/tty`, and `/dev/console` character devices, read-only memory-backed files for boot artifacts, boot-module user programs, the AHCI-read `/disk/bootsector.bin`, a userspace-visible mount table, a recursive read-only FAT16 mount under `/mnt/boot` with disk-backed metadata flags, writable 4 KiB RAM files/directories under existing parents, 32 live RAM-file slots, non-empty directory removal checks, CPU/SMP diagnostics exposed through `/proc/cpu/summary` and `/proc/cpu/topology`, IRQ/APIC diagnostics exposed through `/proc/irq/summary` and `/proc/interrupts`, terminal/framebuffer diagnostics exposed through `/proc/tty/summary`, RAM-mutation success/rejection/write-clipping counters exposed through `/proc/fs/vfs`, and reusable open-file handles.
- `userspace`: init ELF loader, process records with parent PIDs, inherited current directories, environments, and standard file descriptors, normalized cwd-relative path resolution, user-thread records with runnable/running/blocked/exited states, process-level stopped state for job control, exported block-reason/wait-target snapshots, pipe read/write wait metadata, process-wait metadata including specific-child and any-child waits, tick-based sleep metadata, process-wait wake completion that writes the exit code or wait-any result into the waiter address space and resumes the blocked syscall with success, aggregate block/wake/preemption-gate diagnostics, explicit save/restore of full user register context, current process/thread context snapshots, interrupted CPL3 frame capture for resumable user-thread contexts, lifecycle state transitions, exit-code and termination-reason storage, wait/reap metadata, process-local file descriptor tables including VFS files with tracked paths/offsets and bounded pipe endpoints for stdin/stdout/stderr, pipe live-reader/live-writer state with would-block reporting and reader/writer wakeups, owned user-page tracking, user PML4 creation, user stack mapping, spawn-from-ELF process creation, round-robin user-thread selection metadata, and launch-context construction.
- `syscall`: kernel-side syscall dispatcher with shared ABI numbers used by CPL3 init, including scheduler/process introspection, compact current user/kernel identity snapshots, scheduler runtime stats with current Local-APIC-derived CPU ID, indexed process/thread snapshots, startup 64-byte argument metadata, 80-byte environment value metadata and environment mutation, current process/thread/cwd metadata, RTC date/time export, launch-context export, user-scheduler snapshot and next-thread selection, interrupt-frame based CPL3 `Yield` switching with full register preservation, scheduler-backed child waits, post-exit scheduling to another runnable user thread, PMM memory stats, framebuffer info, device inventory counts, indexed and class-filtered device records, boot-block-device stats and read-sector calls, mount-table enumeration, pipe-table enumeration, process-created `/dev/tty` stdin/stdout/stderr descriptors, fd-style terminal I/O with process-scoped overwrite and append redirection, pipe creation and child fd endpoint attachment, terminal viewport and foreground process-group control, VFS node enumeration, direct cwd-relative directory-entry reads, generated virtual `/proc` files for memory, uptime, process list, mount table, filesystem list, boot command line, current-process status, current fd-table snapshots, and per-process `/proc/<pid>/status`, `/proc/<pid>/stat`, `/proc/<pid>/maps`, and `/proc/<pid>/fd` snapshots, `/proc/self/fd/N` and `/proc/<pid>/fd/N` readlink resolution, character-device file descriptor reads/writes, process exit/kill/wait/reap, raw VFS handles, process-local file descriptor calls with cwd-relative path resolution and seek, 4 KiB RAM file/directory mutation calls, and quote-aware command-line spawn-from-VFS-ELF process creation.
- `terminal`: serial plus scrollback-backed framebuffer terminal output backend used by the user `Write` and `TerminalControl` syscalls, with raw/canonical input queues, queue high-water tracking, dropped-input accounting, write counters, scroll-operation counters exposed through `/proc/tty/summary`, and a prompt-line reset primitive for stable shell repainting.
- `userland`: freestanding `init.elf` with syscall wrappers, a boot-scripted shell proof, concurrent APIC-scheduled external-command pipelines, and an interactive keyboard-driven command loop running at CPL3.
- `console` and `log`: GOP framebuffer text output with repaintable scrollback, viewport-follow state, render/glyph/cursor/scroll/reset-line counters exposed through `/proc/tty/summary`, boot-tested viewport and input-line repaint invariants, COM1 serial output, log levels, panic/assert helpers.

The modules are intentionally small but real. They establish stable interfaces
for SMP startup, preemption, address spaces, PCI discovery, and process loading
without faking those milestones as complete.

## Runtime Self-Tests

The kernel runs safe boot self-tests after subsystem initialization:

- BootInfo validation.
- PMM single-page and contiguous allocation/free plus zero-count allocation rejection, invalid-free accounting, and peak-used diagnostics.
- VMM map/write/read/translate/unmap plus range mapping, duplicate-map rejection, unaligned-map rejection, absent-unmap rejection, and shootdown accounting.
- User address-space creation and user-page translation.
- Heap allocation, alignment, write/read, and free.
- ACPI table discovery when RSDP is present, including MCFG ECAM metadata when firmware exposes it.
- ACPI legacy IRQ to GSI mapping, IOAPIC coverage for early IRQ routes, route-helper accounting, vector dispatch accounting, and EOI accounting.
- Masked Local APIC timer LVT configuration/readback plus countdown probe validation.
- Scheduler kernel-thread creation.
- Syscall dispatch error handling, debug-log path, and scalar scheduler/process introspection calls.
- CMOS RTC date/time syscall dispatch with range validation for `/bin/date.elf`.
- Scheduler runtime-stat syscall dispatch for thread-state counts, switch/yield/preempt counters, online CPU count, and current CPU ID derived from the executing Local APIC.
- CPU topology syscall dispatch for MADT-derived CPU/APIC IDs, enabled flags, bootstrap CPU, online CPU count, startup-attempt state, scheduler-capable CPU state, parked AP state, descriptor-ready state, per-CPU Local APIC timer-ready state, AP bootstrap-work completion, AP fixed-IPI work completion, repeated AP queued-work completion, AP-side TLB invalidate completion, and VMM-triggered remote shootdown completion.
- Indexed process and user-thread snapshot syscall dispatch for future init/service-manager enumeration, including per-process/per-thread syscall accounting, last-syscall metadata, Local-APIC user tick runtime accounting, user-scheduler switch/preempt counters, user-thread block reason, and pipe/process wait targets.
- Startup argument/environment count, indexed entry, set, unset, and inherited child environment syscall dispatch for init-style argv/envp metadata.
- Current process ID, current process/thread context, and per-process current-directory syscall dispatch, with absolute and relative directory changes validated by VFS.
- Launch-context syscall dispatch for validated RIP, RSP, CR3, user selectors, and RFLAGS.
- User-thread frame-save validation so an interrupted CPL3 RIP/RSP and general-purpose register state can become the next launch context for resumable user scheduling.
- PMM memory-stat syscall dispatch with total/free/used page and physical-range accounting.
- Device inventory count, indexed device-info, and class-filtered device-info syscall dispatch for future userspace discovery.
- Framebuffer console geometry, repaint, glyph, cursor, and viewport-follow accounting.
- Boot block-device stat and sector-read syscall dispatch, including LBA0 boot-sector signature validation through the shared ABI and kernel-side cache/multi-read accounting.
- Mount-count and indexed mount-info syscall dispatch for `/` and the read-only FAT16 `/mnt/boot` mount.
- Pipe-count and indexed pipe-info syscall dispatch for live pipe capacity, queued-byte, read-offset, and reader/writer endpoint snapshots.
- Framebuffer info syscall dispatch for future userspace display discovery.
- VFS node-count, indexed node-info, direct directory-entry, and path-based stat-info syscall dispatch for namespace discovery and metadata reporting.
- VFS stat/read/open/read-handle/close syscall dispatch and process-local open/read/seek/close syscall dispatch against `/user/init.elf`, including relative `init.elf` resolution from `/user`.
- VFS RAM directory creation/removal syscall dispatch, including relative paths, parent validation, and refusal to remove non-empty directories.
- Userspace process lifecycle transitions from created to runnable, optionally stopped/resumed for job control, then exited and reaped, with exit-code retention before reap and fixed-table slot reuse after reap.
- Process-local file descriptor open/read/close backed by VFS handles on the current user process derived from the selected user thread.
- Standard fd `0`, `1`, and `2` start as `/dev/tty` VFS descriptors, with fd `0` terminal-input reads and fd `1`/`2` terminal writes through the shared syscall ABI. The terminal layer owns raw key delivery for shell editing, canonical line buffering for future cooked readers, terminal-control input-mode switching, queue overflow/drop accounting, high-water tracking, and scroll-operation accounting, including child-process stdin/stdout/stderr overwrite and append redirection to VFS files, framebuffer scrollback viewport control, bounded kernel pipe handoff between spawned user processes, pipe empty/full `WouldBlock` status when a peer endpoint is still live, trap-frame switching that blocks pipe waiters until peer reads/writes make progress, direct blocked pipe-read completion into the suspended reader address space when peer writes add data, blocked-reader EOF completion when the last writer disappears, and direct blocked pipe-write completion from the suspended writer address space when peer reads free capacity.
- Spawn syscall creation of ELF-backed processes from `/bin` with parsed child argv metadata, parent PID assignment, parent CWD inheritance, and a start-suspended flag used by the shell before descriptor setup or explicit `run`.
- User-scheduler snapshot and next-runnable-thread launch-context syscall dispatch, including schedulable-thread counts, fixed user-timeslice quantum, current slice ticks, and expired-slice accounting.
- Userspace scheduler diagnostics for pipe read/write blocks, pipe read/write wakes, specific-child and any-child wait blocks, wait wakes, sleep blocks/wakes, preemption-gate enable/disable events, and APIC preempt switches.
- Cooperative CPL3 `Yield` switching between the shell and `/bin/uyield.elf`, including child resume after a second yield and scheduler handoff after child `Exit`.
- Local APIC timer-driven CPL3 preemption between the shell and `/bin/ubusy.elf`, where the child does not call `Yield`. The userspace manager owns the preemption gate: process/thread lifecycle transitions recompute whether more than one user thread is schedulable, arm the Local APIC system tick when needed, enable user preemption under contention, and drop only the user-preemption gate when contention disappears.
- Local APIC timer-backed CPL3 sleep blocking. `SleepTicks` arms the Local APIC system tick when needed, parks the calling user thread with a wake tick, and the vector `0x40` interrupt path wakes sleepers before returning to or switching between runnable user contexts.
- User execution-context save, child activation, and parent restoration checks.
- Scheduler-backed child process execution where shell `run`/`Wait` blocks the parent, switches into `/bin/hello.elf`, and child `Exit` wakes the parent shell thread.
- Parent-scoped SIGTERM/SIGKILL-like kill, process-group kill, process-group stop/continue, foreground process-group set/get, wait, and reap syscall dispatch for spawned user process records, including process-info termination-reason reporting.
- Trap-frame based process wait blocking: live child waits mark the caller blocked, switch to another runnable CPL3 thread, write the exit code or `WaitAny` result to the waiter when a child exits, and resume the original wait syscall with success.
- User image/stack data-page ownership tracking plus owned user-half page-table teardown and user-thread slot reclamation during process reap.
- User-thread metadata creation for ELF-backed process entry, stack, and PML4 state.
- User launch-context validation for RIP, RSP, CR3, CS, SS, and RFLAGS.
- IOAPIC mask-all setup and ACPI-aware masked route programming/readback for legacy IRQ migration groundwork, with boot-logged route prepare/match counters.
- PCI scan coverage, ECAM-preferred config-path read/write accounting when MCFG is available, command-enable success accounting, ECAM vendor/device cross-checking, read-only PCI config-space probe/fuzz accounting, malformed config-access rejection, class-counter validation, BAR resource discovery, command/status readback, AHCI/e1000/VGA driver-candidate matching, and PCI driver-binding validation.
- Driver-manager lifecycle validation, PCI binding import checks, per-kind imported-device accounting, bus-master requirement accounting, and command-bit union validation.
- AHCI controller binding, ABAR metadata, required command-bit validation, HBA global register reads, implemented-port bitmap parsing, active-port status sampling, command/FIS/table buffer programming, read-only ATA IDENTIFY completion, reusable READ DMA EXT sector reads, and LBA0 boot-sector signature validation.
- Boot block-cache initialization, repeated LBA0 hit/miss validation, bounded multi-sector read validation, split null/zero/oversized invalid-read rejection, backend-failure accounting, cache fill/occupancy tracking, and eviction accounting.
- e1000 adapter binding, MMIO/IO resource metadata, required command-bit validation, CTRL/STATUS/EECD/CTRL_EXT/MDIC/ICR/IMS sampling, decoded link-up/full-duplex/link-speed/bus-width status, interrupt mask/ack setup, receive-address slot 0 MAC metadata, PCI command enablement, RX descriptor buffers, TX descriptors, reusable TX packet buffers, RCTL/TCTL/TIPG programming, RDBA/RDLEN/TDBA/TDLEN readback validation, TX ring cursor/reclaim/null/length diagnostics, RX idle descriptor ownership validation, reusable empty/non-empty `poll_receive` path, and a bounded 60-byte TX smoke with descriptor-done polling through the reusable transmit path.
- VGA adapter binding, MMIO resource metadata, and required command-bit validation.
- Unified device inventory counts and resource validation for storage, network, and display probes.
- VFS namespace lookup, direct child enumeration for `/`, `/bin`, `/proc`, `/proc/block`, `/proc/driver`, `/proc/pci`, `/proc/irq`, `/proc/tty`, `/proc/cpu`, `/proc/self`, `/proc/fs`, dynamic `/proc/<pid>` and `/proc/<pid>/fd` directories, and `/dev`, generated `/proc/meminfo`, `/proc/uptime`, `/proc/stat` scheduler and userspace blocking diagnostics, `/proc/block/bootdisk` block-cache diagnostics, `/proc/driver/summary` driver-manager diagnostics, `/proc/driver/devices` bound-driver records, `/proc/pci/summary` PCI diagnostics, `/proc/pci/devices` retained PCI device rows, `/proc/irq/summary` IRQ/APIC diagnostics, `/proc/interrupts` vector table diagnostics, `/proc/tty/summary` TTY/framebuffer diagnostics, `/proc/cpu/summary` and `/proc/cpu/topology` CPU/SMP diagnostics, `/proc/processes`, `/proc/mounts`, `/proc/filesystems`, `/proc/fs/vfs` RAM-filesystem diagnostics, `/proc/cmdline`, `/proc/self/status`, `/proc/self/stat`, `/proc/self/maps`, `/proc/self/fd`, `/proc/<pid>/status`, `/proc/<pid>/stat`, `/proc/<pid>/maps`, `/proc/<pid>/fd`, and `/proc/<pid>/fd/<n>` virtual-file reads, character-device reads/writes for `/dev/null`, `/dev/zero`, `/dev/tty`, and `/dev/console`, terminal raw/canonical/overflow input tests plus scrollback-control accounting through terminal device handles, read-only `/user/init.elf` ELF-header reads, AHCI-backed `/disk/bootsector.bin` signature validation, recursive FAT16 `/mnt/boot/kernel.elf`, `/mnt/boot/bin/hello.elf`, and `/mnt/boot/user/init.elf` disk-backed ELF-header reads, handle cursor advancement, and close cleanup.
- Cooperative `yield`.
- Local APIC timer-driven preemptive switching between two worker threads.

The kernel halts after tests by design.

## Scheduler Boundary

The scheduler now creates thread objects, maps kernel stacks, performs a
cooperative `yield`, and switches from Local APIC timer interrupts. The boot
demo requires two kernel worker threads to reach target counters under APIC
timer preemption before continuing into CPL3 userspace.

Remaining scheduler work:

- Cleanly return to the original boot context after all worker threads exit.
- Add a proper thread-reaper path and stack reclamation.
- Replace the temporary AP park loop and bootstrap AP work queue with scheduler participation for online non-bootstrap CPUs.
- Extend shootdown coverage from the kernel VMM root to separate user address-space page-table updates.
- Extend the APIC/IPI IST path with explicit nested interrupt accounting, broader per-vector stack policy, and safe remote shootdown handling during active Local APIC timer preemption windows.
- Continue hardening scheduler-owned user timeslicing policy with richer quantum selection and fairness rules beyond the current fixed quantum.

## Userland Boundary

The kernel now has ring-3 code/data selectors, a loaded TSS with `rsp0`, a DPL3
`int 0x80` gate, a syscall dispatcher, user address-space creation, a
freestanding `init.elf` artifact, bootloader handoff of that image, and a kernel
ELF loader that maps PT_LOAD segments plus an initial user stack into a process
address space. The validated launch context is available through the shared
syscall ABI and is executed with `iretq` as the final boot phase. `init.elf`
contains the first shell command dispatcher and QEMU verification requires its
serial output.

Remaining userland work:

- Finish replacing compatibility active-process aliases with current-process/current-thread terminology in older internals.
- Finish user scheduler integration for remaining blocking objects; the shell now runs normal external commands, pipeline stages, foreground jobs, and `run` through the kernel wait-blocked user scheduler path, live child `Wait` can block and wake in the kernel, syscall process identity is derived from the current user thread, and the older bounded `RunProcess` escape hatch has been retired from interrupt dispatch.
- Full terminal line discipline, descriptor duplication beyond inherited stdio, POSIX-style pipe flags, and pipeable stdin/stdout semantics beyond the current bounded blocking pipe path.
- Process exit from CPL3 and scheduler integration for returning from user tasks.
