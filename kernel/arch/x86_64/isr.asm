; Interrupt/exception entry stubs (NASM, 64-bit).
; Each stub pushes a (dummy) error code and the vector number, then jumps to
; a common path that saves all GPRs and calls the C handler with rsp as the
; registers_t pointer. Tables of stub addresses are exported for idt.c.

extern isr_handler
extern irq_handler

section .text

; Exceptions that push a real error code: 8, 10, 11, 12, 13, 14, 17, 21, 29, 30.
%macro ISR_NOERR 1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

%macro IRQ_STUB 1
irq_stub_%1:
    push qword 0
    push qword (%1 + 32)
    jmp irq_common
%endmacro

%assign i 0
%rep 32
  %if i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30
    ISR_ERR i
  %else
    ISR_NOERR i
  %endif
  %assign i i+1
%endrep

%assign i 0
%rep 32
    IRQ_STUB i
    %assign i i+1
%endrep

isr_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rdi, rsp
    call isr_handler
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16          ; drop vector + error code
    iretq

irq_common:
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rbp
    push rdi
    push rsi
    push rdx
    push rcx
    push rbx
    push rax
    mov rdi, rsp
    call irq_handler
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rsi
    pop rdi
    pop rbp
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    add rsp, 16
    iretq

section .rodata
global isr_stub_table
isr_stub_table:
%assign i 0
%rep 32
    dq isr_stub_%+i
    %assign i i+1
%endrep

global irq_stub_table
irq_stub_table:
%assign i 0
%rep 32
    dq irq_stub_%+i
    %assign i i+1
%endrep
