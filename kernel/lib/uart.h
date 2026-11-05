/* Minimal 16550 UART (COM1) driver — our debug output before we have any
 * real console. QEMU exposes it with `-serial stdio` / `-serial file:...`. */
#ifndef MIRIX_LIB_UART_H
#define MIRIX_LIB_UART_H

#include <stdint.h>

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);

#endif
