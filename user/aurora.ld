/* User program link script: classic low-half load address. */
OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0x400000;
    .text   : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data   : { *(.data*) }
    /* Phase 7d: C++ global constructors (aurora links mixtk/editor/settings) */
    .init_array : {
        __init_array_start = .;
        *(.init_array*)
        __init_array_end = .;
    }
    .bss    : { *(.bss*) *(COMMON) }
}
