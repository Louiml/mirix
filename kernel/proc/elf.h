/* Minimal ELF64 loader: maps PT_LOAD segments of an in-memory ELF image
 * (currently the embedded hello.elf) into user virtual addresses. */
#ifndef MIRIX_PROC_ELF_H
#define MIRIX_PROC_ELF_H

#include <stdint.h>
#include <stddef.h>

#define USER_CODE_BASE 0x400000ULL
#define USER_STACK_TOP 0x800000ULL
#define USER_STACK_PAGES 4

/* Loads `blob` (an ELF64 executable), maps its segments + a user stack.
 * Returns the entry point, or 0 on error. */
uint64_t elf_load(const uint8_t *blob, size_t len);

#endif
