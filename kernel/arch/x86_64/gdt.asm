; GDT/TSS loading helpers (NASM, 64-bit).
; gdt_flush(rdi = &gdtr): lgdt, reload all data segments, far-return into CS=0x08.
; tss_load(di = selector): ltr.

section .text

global gdt_flush
global tss_load

gdt_flush:
    lgdt [rdi]
    mov ax, 0x10          ; SEL_KDATA
    mov ds, ax
    mov es, ax
    mov fs, ax
    ; NOTE: do NOT reload gs here — loading a selector would reset the GS
    ; base to 0 and clobber the per-CPU base set via the GS_BASE MSR (SMP).
    mov ss, ax
    push qword 0x08       ; SEL_KCODE
    lea rax, [rel .done]
    push rax
    o64 retf              ; 64-bit far return: reloads CS
.done:
    ret

tss_load:
    ltr di
    ret
