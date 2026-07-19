# Security Policy

IanOS is an early-stage hobby operating system. Treat every build as experimental
and run it only in an isolated VM unless you are intentionally doing hardware
bring-up.

## Reporting

Open a private report or issue with:

- affected component
- build commit or artifact hash
- reproduction steps
- expected and actual behavior
- serial log or QEMU command line when relevant

Do not publish working exploit chains for unfixed issues in shared channels.

## Scope

In scope:

- bootloader memory corruption
- kernel privilege boundary bugs
- syscall validation bugs
- filesystem or ELF loader parsing bugs
- user/kernel isolation failures

Out of scope for now:

- denial of service from trusted kernel code
- missing production hardening in unimplemented subsystems
- issues requiring unsupported hardware configurations
