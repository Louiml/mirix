#include "thread.h"
#include "mm/kmalloc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "drivers/timer.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "lib/panic.h"

#define MAX_THREADS        16
#define THREAD_STACK_SIZE  (16 * 1024)
#define SCHED_QUANTUM_TICKS 4      /* 40 ms at 100 Hz */

typedef enum { TH_RUNNING, TH_READY, TH_DEAD } th_state_t;

typedef struct {
    int id;
    char name[24];
    th_state_t state;
    registers_t saved;       /* full interrupt frame used to resume */
    uint8_t *stack_base;     /* NULL for the boot ("main") thread */
    thread_fn_t entry;
} thread_t;

static thread_t g_threads[MAX_THREADS];
static int g_nthreads;
static thread_t *g_current;
static volatile uint64_t g_switches;

const char *current_thread_name(void)
{
    return g_current ? g_current->name : "?";
}

uint64_t sched_switches(void) { return g_switches; }

/* Entry point for brand-new threads: runs on their fresh stack. */
static void thread_trampoline(void)
{
    thread_fn_t fn = g_current->entry;
    fn();
    /* returned: park the thread forever; the scheduler skips TH_DEAD */
    cli();
    g_current->state = TH_DEAD;
    uart_printf("[%s] exited\n", g_current->name);
    sti();
    for (;;)
        hlt();
}

int thread_create(const char *name, thread_fn_t fn)
{
    cli();
    if (g_nthreads >= MAX_THREADS) {
        sti();
        return -1;
    }
    thread_t *t = &g_threads[g_nthreads];
    t->id = g_nthreads;
    memset(t->name, 0, sizeof(t->name));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->state = TH_READY;
    t->entry = fn;
    t->stack_base = kmalloc(THREAD_STACK_SIZE);
    if (!t->stack_base) {
        sti();
        return -1;
    }

    /* Fabricate the resume frame. When this thread is first switched to,
     * the frame is copied over the live IRQ frame and iretq pops it:
     * rip=trampoline, rsp=top of the new stack, IF set. */
    uint64_t sp = ((uint64_t)t->stack_base + THREAD_STACK_SIZE) & ~15ULL;
    memset(&t->saved, 0, sizeof(t->saved));
    t->saved.rip = (uint64_t)thread_trampoline;
    t->saved.cs = SEL_KCODE;
    t->saved.rflags = 0x202;          /* IF=1 */
    t->saved.rsp = sp;
    t->saved.ss = SEL_KDATA;

    g_nthreads++;
    sti();
    uart_printf("sched: created thread '%s' (id %d, stack %p)\n", t->name, t->id, (void *)sp);
    return t->id;
}

void sched_init(void)
{
    memset(g_threads, 0, sizeof(g_threads));
    g_nthreads = 1;
    g_current = &g_threads[0];
    g_current->id = 0;
    strcpy(g_current->name, "main");
    g_current->state = TH_RUNNING;
    g_current->stack_base = 0;        /* boot stack; frame saved on first switch-out */
    g_switches = 0;
    uart_printf("sched: round-robin ready (%d threads, quantum %d ticks)\n",
                g_nthreads, SCHED_QUANTUM_TICKS);
}

void sched_switch_from_irq(registers_t *frame)
{
    thread_t *cur = g_current;
    if (!cur)
        return;

    /* Save the current thread's full state (force IF on for the resume). */
    cur->saved = *frame;
    cur->saved.rflags |= 0x200;
    if (cur->state == TH_RUNNING)
        cur->state = TH_READY;

    /* Round-robin: first READY thread after the current one. */
    thread_t *next = 0;
    for (int i = 1; i <= g_nthreads; i++) {
        thread_t *t = &g_threads[(cur->id + i) % g_nthreads];
        if (t->state == TH_READY) {
            next = t;
            break;
        }
    }
    if (!next)
        return;                        /* nobody else runnable: keep cur */

    next->state = TH_RUNNING;
    *frame = next->saved;              /* iretq will now resume `next` */
    g_current = next;
    g_switches++;
}

void sched_tick(registers_t *frame)
{
    if (timer_ticks() % SCHED_QUANTUM_TICKS == 0)
        sched_switch_from_irq(frame);
}
