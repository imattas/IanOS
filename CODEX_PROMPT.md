# Codex Task Prompt

You are creating a complete operating system from scratch.

You have been given:

- AGENTS.md
- DESIGN.md

Read both files before making any changes.

They define the architecture, restrictions, and goals.

---

# Environment

This machine has Windows UAC disabled.

If an operation genuinely requires administrator privileges, you may use elevated access.

Prefer non-administrator solutions when possible.

---

# Task

The repository currently has no implementation.

Create the entire operating system project from zero.

You must:

- Create the complete directory structure.
- Create build systems.
- Implement the custom UEFI bootloader.
- Implement the x86_64 hybrid kernel.
- Make it boot through QEMU + OVMF.

---

# Restrictions

Do not use:

- GRUB
- Limine
- Multiboot
- Existing bootloader frameworks

The boot chain must be written from scratch.

---

# Development Plan

Begin with:

1. Project structure
2. Toolchain setup
3. CMake configuration
4. UEFI application
5. ELF64 kernel loader
6. Kernel entry point
7. Basic output

Then implement:

8. GDT
9. IDT
10. Paging
11. Physical memory manager
12. Virtual memory manager
13. APIC
14. SMP startup
15. Scheduler
16. Driver framework
17. Userspace foundation

---

# Expectations

Write real implementations.

Do not create a fake OS demo.

The goal is a real expandable operating system.

Maintain:

- Clean architecture
- Good documentation
- Stable interfaces
- Buildable milestones

After each major milestone:

- Verify compilation.
- Verify QEMU boot.
- Fix issues before continuing.

The final result should be a complete x86_64 UEFI-based operating system built entirely from scratch.