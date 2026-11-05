; User-mode C runtime entry. NASM, 64-bit, linked at 0x400000.
extern main

global _start

section .text
_start:
    call main
    mov rdi, rax          ; exit code = main's return value
    mov rax, 1            ; SYS_exit
    syscall
.hang:
    hlt
    jmp .hang
