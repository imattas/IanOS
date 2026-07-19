bits 64

section .text
global enter_user_mode

; void enter_user_mode(uint64_t rip, uint64_t rsp, uint64_t cr3,
;                      uint64_t rflags, uint64_t cs, uint64_t ss)
enter_user_mode:
    cli
    mov r11, r9
    mov r10, r8
    lea rsp, [rel user_transition_stack_top]
    and rsp, -16
    push r11
    push rsi
    push rcx
    push r10
    push rdi
    mov rax, rdx
    mov cr3, rax
    mov ax, r11w
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    iretq

section .bss
align 16
user_transition_stack:
    resb 16384
user_transition_stack_top:
