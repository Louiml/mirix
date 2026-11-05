; Syscall entry (syscall/sysret) + ring-3 entry (iretq).
;
; Syscall ABI (our own):
;   rax = syscall number, rdi rsi rdx r10 r8 r9 = args 1..6
;   rcx = user rip, r11 = user rflags (destroyed by SYSCALL, per the ISA)
;   return value in rax
;
; On entry IF is already cleared (SFMASK masks it), so we can safely swap
; stacks before calling C.

extern syscall_dispatch

section .bss
align 16
g_syscall_stack:   resb 16384
align 16
g_syscall_stack_top:
align 16
g_ring0_stack:     resb 16384        ; TSS rsp0: stack for ring3->ring0 IRQs/exceptions
align 16
g_ring0_stack_top:

g_user_rsp:     resq 1
g_user_rip:     resq 1
g_user_rflags:  resq 1

section .text

global syscall_entry
global enter_usermode
global ring0_stack_top

ring0_stack_top:
    dq g_ring0_stack_top

; void syscall_entry(void) — LSTAR target
syscall_entry:
    mov [rel g_user_rsp], rsp
    mov [rel g_user_rip], rcx
    mov [rel g_user_rflags], r11
    lea rsp, [rel g_syscall_stack_top]

    ; SYSCALL only guarantees rcx/r11; preserve the argument registers
    ; across the C call (Linux-style: all GPRs except rax/rcx/r11 survive).
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; shuffle syscall args into SysV positions:
    ; dispatch(nr, a1, a2, a3, a4, a5, a6, urip, urflags, urs)
    mov r11, r9          ; a6 -> temp (r11 no longer needed)
    mov r9,  r8          ; a5 -> arg6
    mov r8,  r10         ; a4 -> arg5
    mov rcx, rdx         ; a3 -> arg4
    mov rdx, rsi         ; a2 -> arg3
    mov rsi, rdi         ; a1 -> arg2
    mov rdi, rax         ; nr -> arg1
    push qword [rel g_user_rsp]      ; arg10
    push qword [rel g_user_rflags]   ; arg9
    push qword [rel g_user_rip]      ; arg8
    push r11                         ; arg7 = a6
    call syscall_dispatch
    add rsp, 32

    ; restore the user's argument registers (rax keeps the return value)
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi

    ; restore user context and go back to ring 3
    mov rcx, [rel g_user_rip]
    mov r11, [rel g_user_rflags]
    mov rsp, [rel g_user_rsp]
    o64 sysret

; void enter_usermode(uint64_t entry /*rdi*/, uint64_t user_sp /*rsi*/)
; Builds an iret frame and drops to ring 3. Does not return (until exit).
enter_usermode:
    push qword 0x1b        ; ss = SEL_UDATA | RPL3  (0x18 | 3)
    push rsi               ; rsp
    push qword 0x202       ; rflags (IF=1)
    push qword 0x23        ; cs = SEL_UCODE | RPL3  (0x20 | 3)
    push rdi               ; rip
    iretq
