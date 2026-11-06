; User-mode C runtime entry. NASM, 64-bit, linked at 0x400000.
extern main
extern __init_array_start
extern __init_array_end

section .text
global _start
_start:
    ; Phase 7d: run C++ global constructors (only aurora.ld defines the
    ; __init_array_* symbols; for C-only programs they are absolute 0 and
    ; the loop body never runs).
    mov rbx, __init_array_start
.ctorloop:
    cmp rbx, __init_array_end
    jae .ctordone
    call qword [rbx]
    add rbx, 8
    jmp .ctorloop
.ctordone:
    call main
    mov rdi, rax          ; exit code = main's return value
    mov rax, 1            ; SYS_exit
    syscall
.hang:
    hlt
    jmp .hang
