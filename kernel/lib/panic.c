#include "panic.h"
#include "arch/x86_64/cpu.h"
#include "drivers/fb.h"
#include "lib/uart.h"
#include "lib/printf.h"

#include <stdarg.h>

/* kvprintf writes through a putc callback; use a file-scope target buffer. */
static char *g_msg_p;
static char *g_msg_end;
static void msg_sink(void *ctx, char c)
{
    (void)ctx;
    if (g_msg_p < g_msg_end)
        *g_msg_p++ = c;
}

void kpanic(const char *fmt, ...)
{
    cli();
    fb_set_mirror(0);   /* keep the red panic screen clean */

    static char msg[512];
    g_msg_p = msg;
    g_msg_end = msg + sizeof(msg) - 1;
    *g_msg_p = 0;
    va_list ap;
    va_start(ap, fmt);
    kvprintf(msg_sink, 0, fmt, ap);
    va_end(ap);
    *g_msg_p = 0;

    uart_printf("\n*** KERNEL PANIC ***\n%s\n", msg);

    if (fb_ready()) {
        fb_clear(0x8b0000);   /* panic red */
        fb_puts("\n  KERNEL PANIC\n\n");
        fb_puts(msg);
        fb_puts("\n");
        fb_present();   /* double-buffered: push the red screen to the front */
    }

    uart_printf("halting.\n");
    for (;;)
        hlt();
}

void panic_dump_regs(registers_t *r)
{
    cli();
    fb_set_mirror(0);   /* keep the red panic screen clean */
    uart_printf("\n*** KERNEL PANIC: CPU exception ***\n");
    uart_printf("vector=%llu error=0x%llx\n",
                (unsigned long long)r->vector, (unsigned long long)r->error);
    uart_printf("rip=%p cs=%llx rflags=%llx rsp=%p ss=%llx\n",
                (void *)r->rip, (unsigned long long)r->cs,
                (unsigned long long)r->rflags, (void *)r->rsp,
                (unsigned long long)r->ss);
    uart_printf("cr2=%p cr3=%p\n", (void *)read_cr2(), (void *)read_cr3());
    uart_printf("rax=%llx rbx=%llx rcx=%llx rdx=%llx\n",
                (unsigned long long)r->rax, (unsigned long long)r->rbx,
                (unsigned long long)r->rcx, (unsigned long long)r->rdx);
    uart_printf("rsi=%llx rdi=%llx rbp=%llx r8=%llx\n",
                (unsigned long long)r->rsi, (unsigned long long)r->rdi,
                (unsigned long long)r->rbp, (unsigned long long)r->r8);
    uart_printf("r9=%llx r10=%llx r11=%llx r12=%llx\n",
                (unsigned long long)r->r9, (unsigned long long)r->r10,
                (unsigned long long)r->r11, (unsigned long long)r->r12);
    uart_printf("r13=%llx r14=%llx r15=%llx\n",
                (unsigned long long)r->r13, (unsigned long long)r->r14,
                (unsigned long long)r->r15);

    if (fb_ready()) {
        fb_clear(0x8b0000);
        fb_puts("\n  KERNEL PANIC — CPU exception\n\n");
        fb_printf("vector=%llu error=0x%llx\n",
                  (unsigned long long)r->vector, (unsigned long long)r->error);
        fb_printf("rip=%p cs=%llx rflags=%llx\n",
                  (void *)r->rip, (unsigned long long)r->cs,
                  (unsigned long long)r->rflags);
        fb_printf("rsp=%p ss=%llx cr2=%p\n",
                  (void *)r->rsp, (unsigned long long)r->ss, (void *)read_cr2());
        fb_printf("rax=%llx rbx=%llx rcx=%llx rdx=%llx\n",
                  (unsigned long long)r->rax, (unsigned long long)r->rbx,
                  (unsigned long long)r->rcx, (unsigned long long)r->rdx);
        fb_printf("rsi=%llx rdi=%llx rbp=%llx r8=%llx\n",
                  (unsigned long long)r->rsi, (unsigned long long)r->rdi,
                  (unsigned long long)r->rbp, (unsigned long long)r->r8);
        fb_printf("r9=%llx r10=%llx r11=%llx r12=%llx\n",
                  (unsigned long long)r->r9, (unsigned long long)r->r10,
                  (unsigned long long)r->r11, (unsigned long long)r->r12);
        fb_printf("r13=%llx r14=%llx r15=%llx\n",
                  (unsigned long long)r->r13, (unsigned long long)r->r14,
                  (unsigned long long)r->r15);
        fb_present();   /* double-buffered: push the red screen to the front */
    }

    uart_printf("halting.\n");
    for (;;)
        hlt();
}
