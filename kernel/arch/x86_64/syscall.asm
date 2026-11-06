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
extern g_scratch_arr       ; arch/x86_64/smp.c: [cpu] = scratch qword
extern g_kstack_arr        ; arch/x86_64/smp.c: [cpu] = current thread kstack top

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
;
; SMP (phase 8c): we do NOT trust the SYSCALL GS swap here (QEMU's GS-base
; swap proved unreliable across APs). Instead read the local APIC id from
; MMIO and index flat per-CPU arrays:
;   g_scratch_arr[cpu] = user rsp stash
;   g_kstack_arr[cpu]  = current thread's kernel stack top
; Both arrays live in the kernel half (mapped under the user CR3 too).
syscall_entry:
    ; IF=0 (SFMASK). SYSCALL gave rcx=user rip, r11=user rflags; all other
    ; GPRs are live (rax=nr, rdi/rsi/rdx/r10/r8/r9 = args, rbx = callee-saved).
    ; Park all TEN on the USER stack (writable + mapped under the current
    ; CR3); afterwards every register is free as scratch. NOTE: rbx MUST be
    ; parked — the CPUID below clobbers EBX (phase 8c bug: unparked rbx came
    ; back as the CPU's APIC id, corrupting user callee-saved state).
    push rax                        ; [U+72] nr
    push rdi                        ; [U+64]
    push rsi                        ; [U+56]
    push rdx                        ; [U+48]
    push r10                        ; [U+40]
    push r8                         ; [U+32]
    push r9                         ; [U+24]
    push r11                        ; [U+16] user rflags
    push rcx                        ; [U+8]  user rip
    push rbx                        ; [U+0]  rbx   (rsp = U = orig_urs - 80)

    ; CPU id via CPUID leaf 1, EBX[31:24] = initial APIC id. CPUID is not
    ; privileged and clobbers eax/ebx/ecx/edx — all already parked above.
    ; (The LAPIC MMIO ID register is NOT reliable here: HHDM MMIO reads came
    ; back as 0xffffffff on this QEMU, so we don't use it.)
    mov eax, 1
    cpuid
    shr ebx, 24
    and ebx, 7                      ; cpu (MAX_CPUS = 8)
    lea rdx, [rsp+0x50]             ; rdx = original user rsp (urs); rsp still = U
                                    ; (CPUID just clobbered eax/ebx/ecx/edx, so
                                    ;  urs MUST be recomputed from rsp afterwards)
    lea rax, [rel g_scratch_arr]
    mov [rax+rbx*8], rdx            ; g_scratch_arr[cpu] = orig user rsp
    lea rax, [rel g_kstack_arr]
    mov rsp, [rax+rbx*8]            ; switch to this CPU's current kstack

    ; Rebuild the classic frame on the kernel stack (values from the user
    ; stack at U = orig_urs - 80 = rdx - 80):
    ;   k-8=urip, k-16=urflags, k-24=urs, k-32..k-72=rdi,rsi,rdx,r10,r8,r9,
    ;   k-80=rbx
    lea r11, [rdx-80]               ; r11 = U
    push qword [r11+8]              ; k-8  user rip
    push qword [r11+16]             ; k-16 user rflags
    push rdx                        ; k-24 user rsp
    push qword [r11+64]             ; k-32 rdi
    push qword [r11+56]             ; k-40 rsi
    push qword [r11+48]             ; k-48 rdx
    push qword [r11+40]             ; k-56 r10
    push qword [r11+32]             ; k-64 r8
    push qword [r11+24]             ; k-72 r9
    push qword [r11+0]              ; k-80 rbx

    ; Reload the registers the arg shuffle below expects (rsp = k-80).
    mov rbx, [rsp+0]                ; restore user rbx (CPUID clobbered it)
    mov r9,  [rsp+8]
    mov r8,  [rsp+16]
    mov r10, [rsp+24]
    mov rdx, [rsp+32]
    mov rsi, [rsp+40]
    mov rdi, [rsp+48]
    mov rax, [r11+72]               ; nr

    ; shuffle syscall args into SysV positions:
    ; dispatch(nr, a1, a2, a3, a4, a5, a6, urip, urflags, urs)
    mov r11, r9          ; a6 -> temp (r11 no longer needed)
    mov r9,  r8          ; a5 -> arg6
    mov r8,  r10         ; a4 -> arg5
    mov rcx, rdx         ; a3 -> arg4
    mov rdx, rsi         ; a2 -> arg3
    mov rsi, rdi         ; a1 -> arg2
    mov rdi, rax         ; nr -> arg1
    push qword [rsp+56]      ; arg10 = user rsp      (rsp=k-80 -> k-24)
    push qword [rsp+72]      ; arg9  = user rflags   (rsp=k-88 -> k-16)
    push qword [rsp+88]      ; arg8  = user rip      (rsp=k-96 -> k-8)
    push r11                 ; arg7  = a6
    call syscall_dispatch
    add rsp, 32              ; rsp = k-80

    ; restore the user's argument registers + rbx (rax keeps the return value)
    pop rbx
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi                  ; rsp = k-24

    ; restore user context from this process's own kernel stack and go
    ; back to ring 3 (rsp now = k-24: [urs][urflags][urip])
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
