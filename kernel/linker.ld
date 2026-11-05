/* MirixOS kernel linker script (x86_64, higher half).
 *
 * Limine loads the kernel ELF at its link-time virtual addresses and maps
 * them in the higher half, so we link at 0xffffffff80000000 ("-2 GiB").
 * -mcmodel=kernel in the Makefile generates code that addresses this range.
 */
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS
{
    . = 0xffffffff80000000;

    .text : ALIGN(4K) {
        *(.text .text.*)
    }

    .rodata : ALIGN(4K) {
        *(.rodata .rodata.*)
    }

    /* Limine request list. The start/end markers must stay contiguous with
     * the request structs between them, so keep this section in one piece.
     * Page-align it: Limine writes the response pointers into these structs,
     * so the section is writable, and it must not share a 4K page with a
     * read-only segment (Limine rejects such PHDRs). */
    .limine_requests : ALIGN(4K) {
        KEEP(*(.limine_requests))
    }

    .data : ALIGN(4K) {
        *(.data .data.*)
    }

    .bss : ALIGN(4K) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
