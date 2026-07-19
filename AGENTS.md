# AGENTS.md

# Operating System Project Instructions

## Environment Notes

This project is being created in a development environment where Windows UAC is disabled.

If a required operation needs administrator privileges, elevation is available.

Prefer normal user operations when possible and only use elevated access when required for:
- Tool installation
- Driver/toolchain setup
- Virtualization configuration
- System-level configuration

---

# Project Overview

This project is a complete operating system built from scratch.

The goal is to create a modern x86_64 hybrid kernel with a fully custom UEFI bootloader.

The repository begins with no existing source code.

The implementation must be production-style:
- Modular
- Documented
- Maintainable
- Expandable

---

# Core Requirements

## Architecture

Target:

- x86_64
- UEFI firmware
- Long Mode
- 64-bit kernel

---

# Boot System

Create a custom UEFI bootloader.

Do not use:

- GRUB
- Limine
- Multiboot
- Existing OS boot frameworks

The bootloader must:

- Be a native EFI application
- Initialize UEFI services
- Locate the kernel
- Load ELF64 kernel images
- Retrieve memory information
- Detect framebuffer information
- Detect ACPI information
- Prepare kernel boot data
- Exit UEFI boot services
- Transfer execution to the kernel

---

# Kernel Design

The kernel must be a hybrid kernel.

Design goals:

- Monolithic performance
- Modular subsystem architecture
- Clean separation of components

The kernel must eventually support:

- SMP
- Preemptive multitasking
- Virtual memory
- Hardware abstraction
- Drivers
- User processes

---

# Languages

Preferred:

Kernel:
- C++

Low-level:
- NASM Assembly

Build:
- CMake

Compiler:
- Clang/LLVM preferred

---

# Repository Structure

Create:


/
├── AGENTS.md
├── DESIGN.md
├── README.md
├── CMakeLists.txt
├── bootloader/
├── kernel/
├── common/
├── tools/
├── docs/
└── build/


---

# Coding Standards

Follow these rules:

- Write real implementations.
- Avoid unnecessary placeholders.
- Keep code modular.
- Comment complex hardware interactions.
- Document architectural decisions.
- Keep interfaces stable.

---

# Testing

Primary environment:

- QEMU x86_64
- OVMF UEFI firmware

Debugging:

- GDB
- Serial output
- QEMU debugging tools

---

# Development Order

Implement in stages:

1. Project structure
2. Build system
3. UEFI bootloader
4. Kernel loading
5. Kernel entry
6. Output system
7. CPU initialization
8. Memory management
9. Interrupt system
10. SMP
11. Scheduler
12. Drivers
13. Userspace

Each milestone should leave the project buildable.