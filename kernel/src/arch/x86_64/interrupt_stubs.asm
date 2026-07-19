bits 64

section .text
global load_idt
global exception_stub_table
global irq_stub_table
global syscall_interrupt_stub
global apic_timer_interrupt_stub
global smp_ipi_interrupt_stub
global context_switch
extern interrupt_dispatch

load_idt:
    lidt [rdi]
    ret

%macro SAVE_SCRATCH 0
    push rax
    push rbx
    push rdx
    mov rdx, 0xffff800000100020
    mov ebx, [rdx]
    shr ebx, 24
    and ebx, 31
    shl rbx, 3
    mov rax, [rsp + 16]
    lea rdx, [rel syscall_saved_rax]
    mov [rdx + rbx], rax
    mov rax, [rsp + 8]
    lea rdx, [rel syscall_saved_rbx]
    mov [rdx + rbx], rax
    mov rax, rcx
    lea rdx, [rel syscall_saved_rcx]
    mov [rdx + rbx], rax
    mov rax, [rsp]
    lea rdx, [rel syscall_saved_rdx]
    mov [rdx + rbx], rax
    mov rax, rsi
    lea rdx, [rel syscall_saved_rsi]
    mov [rdx + rbx], rax
    mov rax, rdi
    lea rdx, [rel syscall_saved_rdi]
    mov [rdx + rbx], rax
    mov rax, r10
    lea rdx, [rel syscall_saved_r10]
    mov [rdx + rbx], rax
    mov rax, r11
    lea rdx, [rel syscall_saved_r11]
    mov [rdx + rbx], rax
    pop rdx
    pop rbx
    pop rax
%endmacro

%macro LOAD_SCRATCH_INDEX 0
    mov rdx, 0xffff800000100020
    mov ebx, [rdx]
    shr ebx, 24
    and ebx, 31
    shl rbx, 3
%endmacro

%macro RESTORE_SAVED 2
    lea rdx, [rel %1]
    mov rax, [rdx + rbx]
    mov [rsp + %2], rax
%endmacro

%macro NORMALIZE_NOERR 1
    SAVE_SCRATCH
    mov rax, [rsp]          ; rip
    mov rbx, [rsp + 8]      ; cs
    mov rcx, [rsp + 16]     ; rflags
    test rbx, 3
    jz %%kernel_frame
    mov rdx, [rsp + 24]     ; user rsp
    mov r11, [rsp + 32]     ; user ss
    add rsp, 40
    jmp %%push_frame
%%kernel_frame:
    lea rdx, [rsp + 24]     ; interrupted kernel rsp after CPU frame
    mov r11, 0x10
    add rsp, 24
%%push_frame:
    push r11                ; ss
    push rdx                ; rsp
    push rcx                ; rflags
    push rbx                ; cs
    push rax                ; rip
    push qword 0            ; error
    push qword %1           ; vector
    jmp interrupt_common
%endmacro

%macro NORMALIZE_ERR 1
    SAVE_SCRATCH
    mov r10, [rsp]          ; error
    mov rax, [rsp + 8]      ; rip
    mov rbx, [rsp + 16]     ; cs
    mov rcx, [rsp + 24]     ; rflags
    test rbx, 3
    jz %%kernel_frame
    mov rdx, [rsp + 32]     ; user rsp
    mov r11, [rsp + 40]     ; user ss
    add rsp, 48
    jmp %%push_frame
%%kernel_frame:
    lea rdx, [rsp + 32]     ; interrupted kernel rsp after CPU frame
    mov r11, 0x10
    add rsp, 32
%%push_frame:
    push r11
    push rdx
    push rcx
    push rbx
    push rax
    push r10
    push qword %1
    jmp interrupt_common
%endmacro

%macro ISR_NOERR 1
global isr%1
isr%1:
    cli
    NORMALIZE_NOERR %1
%endmacro

%macro ISR_ERR 1
global isr%1
isr%1:
    cli
    NORMALIZE_ERR %1
%endmacro

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR 8
ISR_NOERR 9
ISR_ERR 10
ISR_ERR 11
ISR_ERR 12
ISR_ERR 13
ISR_ERR 14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR 17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR 29
ISR_ERR 30
ISR_NOERR 31

%macro IRQ_STUB 2
global irq%1
irq%1:
    cli
    NORMALIZE_NOERR %2
%endmacro

IRQ_STUB 0, 32
IRQ_STUB 1, 33
IRQ_STUB 2, 34
IRQ_STUB 3, 35
IRQ_STUB 4, 36
IRQ_STUB 5, 37
IRQ_STUB 6, 38
IRQ_STUB 7, 39
IRQ_STUB 8, 40
IRQ_STUB 9, 41
IRQ_STUB 10, 42
IRQ_STUB 11, 43
IRQ_STUB 12, 44
IRQ_STUB 13, 45
IRQ_STUB 14, 46
IRQ_STUB 15, 47

apic_timer_interrupt_stub:
    cli
    NORMALIZE_NOERR 64

smp_ipi_interrupt_stub:
    cli
    NORMALIZE_NOERR 65

global syscall_interrupt_stub
syscall_interrupt_stub:
    cli
    NORMALIZE_NOERR 128

interrupt_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    LOAD_SCRATCH_INDEX
    RESTORE_SAVED syscall_saved_r11, 32
    RESTORE_SAVED syscall_saved_r10, 40
    RESTORE_SAVED syscall_saved_rdx, 88
    RESTORE_SAVED syscall_saved_rcx, 96
    RESTORE_SAVED syscall_saved_rbx, 104
    RESTORE_SAVED syscall_saved_rax, 112
    cmp qword [rsp + 120], 128
    jne .call_dispatch
    RESTORE_SAVED syscall_saved_r11, 32
    RESTORE_SAVED syscall_saved_r10, 40
    RESTORE_SAVED syscall_saved_rdi, 72
    RESTORE_SAVED syscall_saved_rsi, 80
    RESTORE_SAVED syscall_saved_rdx, 88
    RESTORE_SAVED syscall_saved_rcx, 96
    RESTORE_SAVED syscall_saved_rbx, 104
    RESTORE_SAVED syscall_saved_rax, 112
.call_dispatch:
    mov rdi, rsp
    call interrupt_dispatch
    test rax, rax
    jz .restore
    mov rsp, rax
.restore:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq

; void context_switch(uint64_t** old_rsp, uint64_t* new_rsp)
context_switch:
    mov rdx, rsp
    push qword 0x10
    push rdx
    pushfq
    push qword 0x08
    lea rax, [rel .resume]
    push rax
    push qword 0
    push qword 0
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp
    mov rsp, rsi
    jmp interrupt_common.restore
.resume:
    ret

section .rodata
exception_stub_table:
%assign j 0
%rep 32
    dq isr%+j
%assign j j+1
%endrep

irq_stub_table:
%assign k 0
%rep 16
    dq irq%+k
%assign k k+1
%endrep

section .bss
align 8
syscall_saved_rax: resq 32
syscall_saved_rbx: resq 32
syscall_saved_rcx: resq 32
syscall_saved_rdx: resq 32
syscall_saved_rsi: resq 32
syscall_saved_rdi: resq 32
syscall_saved_r10: resq 32
syscall_saved_r11: resq 32
