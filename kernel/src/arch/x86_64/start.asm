bits 64

section .text.start
global _start
extern kernel_main

_start:
    cli
    mov rdi, rcx
    xor rbp, rbp
    lea rsp, [rel boot_stack_top]
    call kernel_main
.halt:
    hlt
    jmp .halt

section .bss
align 16
boot_stack:
    resb 65536
boot_stack_top:
