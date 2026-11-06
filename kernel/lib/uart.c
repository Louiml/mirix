#include "uart.h"
#include "io.h"
#include "printf.h"
#include "spinlock.h"
#include "drivers/fb.h"

#include <stdarg.h>

#define COM1 0x3F8

/* SMP (8c): one lock for the whole line so output from different CPUs never
 * interleaves. g_uart_nolock is set by the panic path (see uart_panic_begin). */
static spinlock_t g_uart_lock = SPINLOCK_INIT;
static volatile int g_uart_nolock;

void uart_panic_begin(void)
{
    g_uart_nolock = 1;
}

/* 16550 register offsets */
#define UART_DATA   0  /* data / divisor latch low  (DLAB=1) */
#define UART_IER    1  /* interrupt enable / divisor latch high (DLAB=1) */
#define UART_FCR    2  /* FIFO control (write) */
#define UART_LCR    3  /* line control */
#define UART_MCR    4  /* modem control */
#define UART_LSR    5  /* line status */

#define LSR_THR_EMPTY 0x20

void uart_init(void)
{
    outb(COM1 + UART_IER, 0x00);   /* disable interrupts */
    outb(COM1 + UART_LCR, 0x80);   /* DLAB=1: divisor latch open */
    outb(COM1 + UART_DATA, 0x01);  /* divisor low  = 1  -> 115200 baud */
    outb(COM1 + UART_IER, 0x00);   /* divisor high = 0 */
    outb(COM1 + UART_LCR, 0x03);   /* DLAB=0, 8 data bits, no parity, 1 stop */
    outb(COM1 + UART_FCR, 0xC7);   /* enable + clear FIFOs, 14-byte trigger */
    outb(COM1 + UART_MCR, 0x0B);   /* DTR + RTS + OUT2 (IRQs routable) */
}

void uart_putc(char c)
{
    if (c == '\n')          /* CRLF so terminals show lines correctly */
        uart_putc('\r');
    while ((inb(COM1 + UART_LSR) & LSR_THR_EMPTY) == 0)
        ;
    outb(COM1 + UART_DATA, (uint8_t)c);
    /* Phase 5b: mirror everything to the scrolling framebuffer console
     * (fb_putc never calls back into uart, so no recursion). */
    if (fb_ready() && fb_mirror())
        fb_putc(c);
}

void uart_puts(const char *s)
{
    if (g_uart_nolock) {
        while (*s)
            uart_putc(*s++);
        return;
    }
    uint64_t fl = spin_lock_irqsave(&g_uart_lock);
    while (*s)
        uart_putc(*s++);
    spin_unlock_irqrestore(&g_uart_lock, fl);
}

void uart_write(const char *buf, unsigned long n)
{
    if (g_uart_nolock) {
        for (unsigned long i = 0; i < n; i++)
            uart_putc(buf[i]);
        return;
    }
    uint64_t fl = spin_lock_irqsave(&g_uart_lock);
    for (unsigned long i = 0; i < n; i++)
        uart_putc(buf[i]);
    spin_unlock_irqrestore(&g_uart_lock, fl);
}

static void uart_sink(void *ctx, char c)
{
    (void)ctx;
    uart_putc(c);
}

void uart_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (g_uart_nolock) {
        kvprintf(uart_sink, 0, fmt, ap);
    } else {
        uint64_t fl = spin_lock_irqsave(&g_uart_lock);
        kvprintf(uart_sink, 0, fmt, ap);
        spin_unlock_irqrestore(&g_uart_lock, fl);
    }
    va_end(ap);
}
