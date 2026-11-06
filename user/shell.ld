/* User program link script: classic low-half load address. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    /* C++ global constructors (harmless for C-only programs: empty) */
    .init_array : {
        __init_array_start = .;
        *(.init_array*)
        __init_array_end = .;
    }
    .bss    : { *(.bss*) *(COMMON) }
}
