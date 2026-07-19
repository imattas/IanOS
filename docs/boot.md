# Boot Architecture

The boot chain is intentionally small and owned by this repository.

1. UEFI firmware loads `EFI/BOOT/BOOTX64.EFI`.
2. The bootloader opens the boot volume through UEFI Simple File System.
3. The bootloader displays a repo-owned text boot manager with normal UEFI,
   recovery UEFI, debug UEFI, and disabled BIOS/legacy entries. Automated
   verifier boots skip the menu when `\boot\runtests` is present. Interactive
   boots wait for an explicit selection and do not use a timeout.
4. `\kernel.elf` is read into loader memory.
5. `\user\init.elf` and `\bin\hello.elf` are read into loader memory and minimally validated as ELF64 x86_64 payloads.
6. Kernel ELF64 program headers are validated and copied to physical memory.
7. The bootloader captures GOP framebuffer, ACPI RSDP, and a converted memory map.
8. The bootloader records kernel physical base/end, kernel entry point, init image base/size, a boot-module table for user ELF payloads, and boot flags.
9. `ExitBootServices()` is retried against a fresh memory-map key if needed.
10. Control transfers directly to the kernel entry point.

No GRUB, Limine, Multiboot, GNU-EFI, or external boot framework is used.

The first kernel is linked at `0x100000` so it can execute immediately after the
UEFI handoff. Higher-half mappings are a VMM milestone rather than a bootloader
assumption.

The kernel now clones the firmware-provided PML4 into a PMM-owned page before
creating heap, APIC, or test mappings. This preserves early identity execution
without mutating OVMF-owned page-table pages.

The kernel also loads a TSS during CPU setup. It provides `rsp0` for privilege
transitions and IST1 for double-fault handling. The final boot phase enters the
init process at CPL3 with an `iretq` frame built from the validated launch
context.

The initial user payload is executed. The kernel validates the handoff, reserves
all boot-module images in the PMM, registers them in VFS, parses init ELF64
program headers, maps its PT_LOAD segments and stack into a user address space,
adds supervisor-only kernel runtime mappings needed for syscall handling, and
enters the ELF entry point. Additional boot modules such as `/bin/hello.elf`
can be spawned into process records through the userspace syscall ABI.

Normal interactive boots do not run the shell validation transcript. The
bootloader sets `kBootFlagRunBootScript` only when `\boot\runtests` exists on
the ESP; `scripts/verify.ps1` creates that marker through `scripts/run.ps1
-BootScript`, while normal `scripts/run.ps1` removes it before launch.

Recovery mode is a bootloader-selected UEFI path, not a separate kernel image.
The menu sets `kBootFlagRecovery` in `BootInfo`; the kernel logs the selected
mode and starts init with `--recovery`, which enters a dedicated rescue target
with a `recovery# ` prompt. Recovery mode does not run the normal shell startup
banner or default command environment; it exposes repair-focused commands,
startup diagnostics, and an explicit `shell` handoff for continuing into normal
userspace after rescue work. The BIOS/legacy entry is currently disabled because
no separate BIOS boot sector or real-mode loader exists in this repository yet.

Debug mode is also bootloader-selected. It sets `kBootFlagDebug`, causing the
kernel's normal `[INFO]` boot log stream to be painted to the framebuffer during
startup. Normal boots still retain the full serial and in-memory kernel log,
but they keep framebuffer output clean until userspace starts.

The bootloader also clears the firmware text output as early as possible,
renders the IanOS icon, and shows a progress bar tied to actual loader stages
before the kernel handoff. The progress UI does not replace the serial or
kernel self-test proof path.
