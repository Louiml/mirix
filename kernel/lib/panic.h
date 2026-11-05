/* Kernel panic: red screen + register dump + serial log, then halt.
 * Does not return. Safe to call before fb_init (serial only). */
#ifndef MIRIX_LIB_PANIC_H
#define MIRIX_LIB_PANIC_H

#include "arch/x86_64/idt.h"

void kpanic(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void panic_dump_regs(registers_t *regs) __attribute__((noreturn));

#endif
