bits 16
default abs
org 0x7000

%define CONFIG_BASE 0x8000
%define CONFIG_GDTR (CONFIG_BASE + 0x00)
%define CONFIG_CR3 (CONFIG_BASE + 0x10)
%define CONFIG_STACK_TOP (CONFIG_BASE + 0x18)
%define CONFIG_CHECKIN_PTR (CONFIG_BASE + 0x20)
%define CONFIG_STATE_PTR (CONFIG_BASE + 0x28)
%define CONFIG_ENTRY_PTR (CONFIG_BASE + 0x30)

start:
    cli
    cld
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x6ff0

    lgdt [CONFIG_GDTR]

    mov eax, cr0
    or eax, 1
    mov cr0, eax
    jmp dword 0x08:protected_entry

bits 32
protected_entry:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, [CONFIG_CR3]
    mov cr3, eax

    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 0x80000000
    mov cr0, eax
    jmp dword 0x18:long_mode_entry

bits 64
long_mode_entry:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax

    mov rax, [CONFIG_STACK_TOP]
    mov rsp, rax

    mov rax, [CONFIG_STATE_PTR]
    mov dword [rax], 0xc0def00d

    mov rax, [CONFIG_CHECKIN_PTR]
    lock inc dword [rax]

    mov rax, [CONFIG_ENTRY_PTR]
    jmp rax

trampoline_end:
