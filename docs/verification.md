# Verification

Use the repository verifier after every kernel, bootloader, or userland ABI
change:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/verify.ps1
```

The verifier performs the following gates:

- checks local toolchain prerequisites
- configures and builds with CMake/Ninja
- validates `BOOTX64.EFI`, `kernel.elf`, `init.elf`, every `/bin/*.elf`
  artifact, and the generated `kernel.img`
- boots QEMU with OVMF in headless mode with the explicit `\boot\runtests`
  marker enabled
- captures serial output in `build/qemu-serial.log`
- requires runtime markers for boot, ACPI table diagnostics, ACPI/APIC/IOAPIC routing, PCI, boot-module
  user program loading, userspace loading, self-tests, CPU topology inventory,
  per-CPU runtime records, guarded SMP INIT/SIPI delivery, AP long-mode
  check-in, AP-side descriptor loading, AP-side masked Local APIC timer setup,
  controlled AP bootstrap work, AP-side fixed-IPI work completion, repeated
  queued AP work completion, AP-side TLB invalidate completion,
  VMM-triggered remote TLB shootdown markers, parked AP state, and online
  topology marking for the non-bootstrap CPU,
  Local APIC scheduler
  preemption, user-thread frame-save validation, PS/2 keyboard setup,
  raw/canonical terminal input self-tests, fd-backed stdin/stdout terminal I/O, CPL3
  init entry, boot-scripted shell output, shell current-context reporting
  through `ctx`, shell `spawn`/`usched`/`nextuser`/`run`/`wait`/`reap`,
  cooperative `/bin/uyield.elf` switching with full
  CPL3 frame restoration, Local APIC timer-driven `/bin/ubusy.elf` preemption
  without child `Yield`, and post-child-exit scheduling, shell `PATH`
  resolution through `which`, shell
  `export`/`unset`, inherited child environment reporting through
  `/bin/env.elf`, selected external environment lookup through
  `/bin/printenv.elf`, 80-byte environment value storage and inheritance, shell
  `$?`/`$VAR`/`${VAR}` expansion in both init shell and
  `/bin/sh.elf`, quote-aware child argv with single quotes, double quotes, and
  escaped spaces, shell exit status reporting plus `&&`/`||` control flow,
  stderr redirection through `2>`/`2>>` with `/bin/err.elf`,
  concurrent APIC-scheduled shell pipelines with spawned pipe stages, kernel
  pipe read/write blocking and wake markers, direct resumed pipe-read byte
  counts, full-pipe resumed write byte counts from `burst | wc`, 4 KiB
  RAM-backed pipeline capture through `burst | tee /tmp/big.txt | wc`, and
  no-reader broken-pipe writer wakeup from `burst | true`,
  built-in and external `stat` metadata for file and directory nodes,
  direct directory-entry enumeration through built-in `ls` and `/bin/ls.elf`,
  generated `/proc/meminfo`, `/proc/iomem`, `/proc/buddyinfo`, `/proc/heapinfo`, `/proc/vmstat`, `/proc/uptime`, `/proc/loadavg`, `/proc/sched_debug`, `/proc/modules`, `/proc/kmsg`, `/proc/block/bootdisk`, `/proc/driver/summary`, `/proc/driver/devices`, `/proc/pci/summary`, `/proc/pci/devices`, `/proc/irq/summary`, `/proc/interrupts`, `/proc/tty/summary`, `/proc/cpu/summary`, `/proc/cpu/topology`, `/proc/net/summary`, `/proc/net/dev`, `/proc/processes`,
  `/proc/mounts`, `/proc/filesystems`, `/proc/fs/vfs`, `/proc/cmdline`,
  `/proc/stat`, `/proc/sys/kernel/hostname`, `/proc/sys/kernel/ostype`,
  `/proc/sys/kernel/osrelease`, `/proc/sys/kernel/version`,
  `/proc/self/status`, `/proc/self/stat`, `/proc/self/maps`, `/proc/self/cmdline`, `/proc/self/environ`, `/proc/self/cwd`, `/proc/self/exe`, `/proc/self/root`, `/proc/self/fd`, `/proc/self/fdinfo`, `/proc/self/limits`, `/proc/1/status`, `/proc/1/stat`, `/proc/1/maps`, `/proc/1/cmdline`, `/proc/1/environ`, `/proc/1/cwd`, `/proc/1/exe`, `/proc/1/root`, `/proc/1/fd`, `/proc/1/fdinfo`, and `/proc/1/limits` virtual-file metadata
  and reads, default `/dev/tty` fd 0/1/2 process descriptors,
  `/proc/self/fd/1` readlink and realpath resolution under stdout redirection,
  `/dev/null`, `/dev/zero`, `/dev/tty`, and `/dev/console` character-device
  metadata plus bounded `/bin/fastfetch.elf`, `/bin/sysctl.elf`, `/bin/lsblk.elf`, `/bin/findmnt.elf`, `/bin/iostat.elf`, `/bin/lsmem.elf`, `/bin/iomem.elf`, `/bin/fbset.elf`, `/bin/lspci.elf`, `/bin/irqstat.elf`, `/bin/interrupts.elf`, `/bin/mmstat.elf`, `/bin/buddyinfo.elf`, `/bin/heapinfo.elf`, `/bin/procvmstat.elf`, `/bin/procstat.elf`, `/bin/netstat.elf`, `/bin/lsmod.elf`, `/bin/cmdline.elf`, `/bin/pcmdline.elf`, `/bin/kmsg.elf`, `/bin/loadavg.elf`, `/bin/scheddebug.elf`, `/bin/devio.elf`, `/bin/tty.elf`, `/bin/ttystat.elf`, `/bin/stty.elf`,
  and `/bin/ttyread.elf` TTY summary streaming, input-mode, read/write, mode-switch, canonical stdin,
  `/bin/clear.elf` terminal-clear execution, and terminal-read idle proof,
  AHCI-backed `/disk/bootsector.bin`, block-cache self-test markers,
  recursive read-only FAT16 `/mnt/boot` mount proof plus userspace-visible mount-table, Linux-shaped mountinfo, and `statfs` output,
  RTC-backed external `date`, argument-backed external `cal`,
  `/etc/hostname`, `/proc/sys/kernel/*` identity files, multi-file external `cat` and `wc`, external `hostname`, external `id`, external `groups`, external `whoami`,
  VFS attribute inspection through `lsattr`,
  VFS path-component resolution through `namei`,
  recursive directory traversal through `tree`,
  path-to-filesystem ownership through `statfs`,
  `basename`, `dirname`, option-bounded `head` and `tail`, and quiet
  `test` predicates chained through `&&`/`||`, external `sort`/`uniq -c`/`uniq -d`/`uniq -u`/`find`/`hexdump`/`od`/`base64`/`which`/`readelf`/`file`/`sha256sum`/`sha224sum`/`sha512sum`/`sha384sum`/`sha1sum`/`md5sum`/`cksum`/`fold`/`printf`/`dd`/`xargs`/`yes`/`cmp`/`strings`/`nl`/`tr`/`sed`/`cut`/`paste`/`rev`/`tac`/`seq`/`expr`/`nproc`/`lscpu`/`schedstat`/`scheddebug`/`vmstat`/`top`/`pstree`/`findmnt`/`mountinfo`/`proccomm`/`proctask`, and
  bounded `sh` script execution from a VFS file,
  process-name lookup through `pgrep` and `pidof`,
  SIGKILL and SIGTERM child termination with distinct wait statuses, shell
  `jobs` table updates, and `&` background external command launch,
  tick-blocked userspace `SleepTicks` wake/resume markers,
  scheduler-driven external command execution with kernel process-wait
  block/wake markers, zero-retry resumed `Wait` syscalls, scheduler-backed shell `run`, `/bin/ps.elf` user-thread
  block-reason/wait-target reporting, child argv reporting from multiple external programs,
  external file-descriptor reads, external VFS directory enumeration, and interactive
  shell readiness
- rejects fatal exception markers and explicit boot-demo failure markers
- confirms no QEMU process remains running

Manual visual runs are still available:

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1
```

or:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run.ps1
```

Normal runs keep QEMU open for interactive shell use. Choose a boot-manager
entry, type commands in the QEMU window after the `ianos> ` prompt, then close
that window to stop the run.
`run.ps1` and `scripts/run.ps1` remove the boot-test marker by default, so they do not run
the long scripted command transcript.

For serial-only manual runs:

```powershell
powershell -ExecutionPolicy Bypass -File run.ps1 -Headless
```

Current expected terminal result:

```text
IanOS verification passed.
```
