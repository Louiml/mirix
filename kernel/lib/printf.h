/* Tiny printf engine with a pluggable output sink, so the same formatter
 * drives the serial port (uart_printf) and the framebuffer (fb_printf).
 * Supports: %c %s %d %i %u %x %X %p %% with l / ll length modifiers. */
#ifndef MIRIX_LIB_PRINTF_H
#define MIRIX_LIB_PRINTF_H

#include <stdarg.h>

typedef void (*mirix_putc_fn)(void *ctx, char c);

int kvprintf(mirix_putc_fn putc, void *ctx, const char *fmt, va_list ap);

#endif
