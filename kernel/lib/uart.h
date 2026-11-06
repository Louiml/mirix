/* Minimal 16550 UART (COM1) driver — our debug output before we have any
 * real console. QEMU exposes it with `-serial stdio` / `-serial file:...`. */
#ifndef MIRIX_LIB_UART_H
#define MIRIX_LIB_UART_H

void uart_init(void);
void uart_putc(char c);
void uart_puts(const char *s);
void uart_write(const char *buf, unsigned long n);   /* raw bytes, no formatting */
void uart_printf(const char *fmt, ...);   /* printf-format, to serial */

/* SMP (8c): uart_puts/write/printf hold a spinlock so lines from different
 * CPUs never interleave. kpanic() calls this first — the panicking CPU may
 * already hold the lock (or another CPU may hold it forever), so the panic
 * path bypasses locking entirely. */
void uart_panic_begin(void);

#endif
