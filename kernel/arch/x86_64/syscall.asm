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
extern g_current_kstack_top   ; proc/thread.c: running process's kernel stack top

section .bss
g_scratch:      resq 1         ; only touched with IF=0 (see below)

section .text

global syscall_entry
global enter_usermode

; void syscall_entry(void) — LSTAR target
;
; The user context (rip/rflags/rsp) is kept ON THE PER-PROCESS KERNEL STACK,
; never in globals: sched_yield()/proc_exit_current() sti+hlt mid-syscall,
; so another process can run a syscall while this one is parked — globals
; would be clobbered and the parked process would sysret to the wrong rip
; under the wrong CR3 (observed as #GP at the ring-3 hlt after exit).
syscall_entry:
    mov [rel g_scratch], rsp        ; stash user rsp; IF=0 here (SFMASK), no race
    mov rsp, [rel g_current_kstack_top]   ; per-process kernel stack
    push rcx                        ; [k+64] user rip
    push r11                        ; [k+56] user rflags
    push qword [rel g_scratch]      ; [k+48] user rsp

    ; SYSCALL only guarantees rcx/r11; preserve the argument registers
    ; across the C call (Linux-style: all GPRs except rax/rcx/r11 survive).
    push rdi                        ; [k+40]
    push rsi                        ; [k+32]
    push rdx                        ; [k+24]
    push r10                        ; [k+16]
    push r8                         ; [k+8]
    push r9                         ; [k+0]

    ; shuffle syscall args into SysV positions:
    ; dispatch(nr, a1, a2, a3, a4, a5, a6, urip, urflags, urs)
    mov r11, r9          ; a6 -> temp (r11 no longer needed)
    mov r9,  r8          ; a5 -> arg6
    mov r8,  r10         ; a4 -> arg5
    mov rcx, rdx         ; a3 -> arg4
    mov rdx, rsi         ; a2 -> arg3
    mov rsi, rdi         ; a1 -> arg2
    mov rdi, rax         ; nr -> arg1
    push qword [rsp+48]      ; arg10 = user rsp      (rsp=k)
    push qword [rsp+64]      ; arg9  = user rflags   (rsp=k-8, k+56)
    push qword [rsp+80]      ; arg8  = user rip      (rsp=k-16, k+64)
    push r11                 ; arg7  = a6
    call syscall_dispatch
    add rsp, 32

    ; restore the user's argument registers (rax keeps the return value)
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi

    ; restore user context from this process's own kernel stack and go
    ; back to ring 3 (rsp now = k+48: [urs][urflags][urip])
    mov rcx, [rsp+16]
    mov r11, [rsp+8]
    mov rsp, [rsp]
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
