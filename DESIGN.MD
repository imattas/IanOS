# Operating System Design Document

## Overview

A complete x86_64 operating system created entirely from scratch.

The system consists of:

- Custom UEFI bootloader
- Hybrid kernel
- Modular hardware layer
- Future userspace environment

---

# Architecture

CPU:


Architecture: x86_64
Mode: Long Mode
Paging: 4-level paging
Endian: Little Endian


---

# Boot Design

## Bootloader

Custom UEFI bootloader.

No:

- GRUB
- Limine
- Multiboot

Responsibilities:

- UEFI startup
- File loading
- ELF64 parsing
- Memory discovery
- Hardware information collection
- Kernel preparation
- Boot handoff

---

# Boot Flow


UEFI Firmware

↓

Custom EFI Bootloader

↓

Load Kernel ELF64

↓

Create Boot Information Structure

↓

Initialize CPU State

↓

ExitBootServices()

↓

Jump To Kernel

↓

Kernel Initialization


---

# Kernel

Type:

Hybrid Kernel

Design:


Fast Kernel Core

Modular Services

Hardware Abstraction


---

# CPU Subsystem

Required:

- GDT
- IDT
- TSS
- MSR support
- CPU feature detection
- SYSCALL/SYSRET

---

# Memory System

## Physical Memory Manager

Initial:

- Bitmap allocator

Future:

- Buddy allocator

---

## Virtual Memory

Support:

- Page tables
- Higher-half kernel
- Address spaces
- Kernel mappings
- User mappings

---

# SMP

Required.

Support:

- Multiple CPUs
- AP startup
- Local APIC
- Per CPU structures
- CPU scheduling

Example:


CPU

├── PerCPU Data
├── Kernel Stack
├── Scheduler State
└── Interrupt State


---

# Interrupt System

Components:

- IDT
- Exceptions
- IRQ handling
- Local APIC
- IO APIC

---

# Scheduler

Initial:

- Preemptive scheduler

Support:

- Kernel threads
- User threads
- CPU affinity

Future:

- Priority scheduling
- Load balancing

---

# Drivers

Initial:

- UEFI framebuffer
- Serial
- PCI
- ACPI
- APIC

Future:

- USB
- NVMe
- AHCI
- Networking
- Audio

---

# Graphics

Initial:

- GOP framebuffer

Future:

- Graphics subsystem
- Window manager
- Hardware acceleration

---

# Filesystems

Boot:

- FAT32 via UEFI

Kernel:

- Virtual filesystem layer

Future:

- Ext filesystem
- Custom filesystem

---

# Executables

Format:

ELF64

Future:

- Processes
- Shared libraries
- Dynamic linker

---

# Final Goal

A complete general-purpose x86_64 operating system featuring:

- Custom boot chain
- SMP kernel
- Multitasking
- Drivers
- Filesystem
- Networking
- Applications