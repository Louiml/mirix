#include "thread.h"
#include "mm/mm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "drivers/timer.h"
#include "proc/elf.h"
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
    uint64_t pml4_phys;      /* address space (CR3 value) */
    uint64_t kstack_top;     /* kernel stack top: rsp0 + syscall entry */
    uint8_t *stack_base;     /* NULL for the boot ("main") thread */
    thread_fn_t entry;       /* ring-0 threads only */
    int is_user;
    struct { int vnode; uint32_t off; } fds[PROC_MAX_FDS];
} thread_t;

static void fds_init(thread_t *t)
{
    for (int i = 0; i < PROC_MAX_FDS; i++) {
        t->fds[i].vnode = (i < 3) ? 0 : -1;   /* 0/1/2 = console */
        t->fds[i].off = 0;
    }
}

static thread_t g_threads[MAX_THREADS];
static int g_nthreads;
static thread_t *g_current;
static volatile uint64_t g_switches;
static volatile int g_force_switch;

uint64_t g_current_kstack_top;

const char *current_thread_name(void)
{
    return g_current ? g_current->name : "?";
}

int current_thread_id(void)
{
    return g_current ? g_current->id : -1;
}

uint64_t sched_switches(void) { return g_switches; }

/* Entry point for brand-new ring-0 threads: runs on their fresh stack. */
static void thread_trampoline(void)
{
    thread_fn_t fn = g_current->entry;
    fn();
    cli();
    g_current->state = TH_DEAD;
    uart_printf("[%s] exited\n", g_current->name);
    sti();
    for (;;)
        hlt();
}

static thread_t *alloc_slot(const char *name)
{
    if (g_nthreads >= MAX_THREADS)
        return 0;
    thread_t *t = &g_threads[g_nthreads];
    t->id = g_nthreads;
    memset(t->name, 0, sizeof(t->name));
    strncpy(t->name, name, sizeof(t->name) - 1);
    t->stack_base = kmalloc(THREAD_STACK_SIZE);
    if (!t->stack_base)
        return 0;
    t->kstack_top = ((uint64_t)t->stack_base + THREAD_STACK_SIZE) & ~15ULL;
    t->pml4_phys = read_cr3() & PTE_ADDR_MASK;   /* shares current space */
    fds_init(t);
    g_nthreads++;
    return t;
}

int thread_create(const char *name, thread_fn_t fn)
{
    cli();
    thread_t *t = alloc_slot(name);
    if (!t) {
        sti();
        return -1;
    }
    t->state = TH_READY;
    t->entry = fn;
    t->is_user = 0;

    /* Fabricate the resume frame: rip=trampoline, rsp=top of new stack. */
    memset(&t->saved, 0, sizeof(t->saved));
    t->saved.rip = (uint64_t)thread_trampoline;
    t->saved.cs = SEL_KCODE;
    t->saved.rflags = 0x202;
    t->saved.rsp = t->kstack_top;
    t->saved.ss = SEL_KDATA;

    uart_printf("sched: created thread '%s' (id %d, stack %p)\n",
                t->name, t->id, (void *)t->kstack_top);
    sti();
    return t->id;
}

int process_spawn_user(const char *name, const uint8_t *elf, size_t len)
{
    cli();
    thread_t *t = alloc_slot(name);
    if (!t) {
        sti();
        return -1;
    }
    t->is_user = 1;
    t->state = TH_READY;

    /* Private address space; load the ELF into it. */
    t->pml4_phys = vmm_create_space();
    uint64_t prev = read_cr3() & PTE_ADDR_MASK;
    write_cr3(t->pml4_phys);
    uint64_t entry = elf_load(elf, len);
    write_cr3(prev);
    if (entry == 0) {
        sti();
        return -1;
    }

    /* Fabricated ring-3 resume frame — iretq drops straight into user code. */
    memset(&t->saved, 0, sizeof(t->saved));
    t->saved.rip = entry;
    t->saved.cs = SEL_UCODE | RPL_USER;    /* 0x23 */
    t->saved.ss = SEL_UDATA | RPL_USER;    /* 0x1b */
    t->saved.rflags = 0x202;
    t->saved.rsp = USER_STACK_TOP;

    uart_printf("sched: spawned user process '%s' (id %d, entry %p, pml4 %p)\n",
                t->name, t->id, (void *)entry, (void *)t->pml4_phys);
    sti();
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
    g_current->stack_base = 0;        /* boot stack */
    uint64_t sp;
    g_current->kstack_top = (uint64_t)&sp & ~15ULL;
    g_current->pml4_phys = read_cr3() & PTE_ADDR_MASK;
    g_current->is_user = 0;
    fds_init(g_current);
    g_current_kstack_top = g_current->kstack_top;
    tss_set_rsp0(g_current->kstack_top);
    g_switches = 0;
    uart_printf("sched: round-robin ready (%d procs, quantum %d ticks)\n",
                g_nthreads, SCHED_QUANTUM_TICKS);
}

void sched_switch_from_irq(registers_t *frame)
{
    thread_t *cur = g_current;
    if (!cur)
        return;

    /* Save the current process's full state (force IF on for the resume). */
    cur->saved = *frame;
    cur->saved.rflags |= 0x200;
    if (cur->state == TH_RUNNING)
        cur->state = TH_READY;

    /* Round-robin: first READY process after the current one. */
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

    /* Cross address space + kernel stack if needed (IF=0: atomic). */
    if (next->pml4_phys != cur->pml4_phys)
        write_cr3(next->pml4_phys);
    tss_set_rsp0(next->kstack_top);
    g_current_kstack_top = next->kstack_top;

    next->state = TH_RUNNING;
    *frame = next->saved;              /* iretq will now resume `next` */
    g_current = next;
    g_switches++;
}

void sched_tick(registers_t *frame)
{
    if (g_force_switch || timer_ticks() % SCHED_QUANTUM_TICKS == 0) {
        g_force_switch = 0;
        sched_switch_from_irq(frame);
    }
}

void sched_yield(void)
{
    /* Called from a syscall (IF=0). Wait for the next timer IRQ, which will
     * switch to another process; on resume we continue right after hlt. */
    g_force_switch = 1;
    sti();
    hlt();
    cli();
    g_force_switch = 0;
}

long sched_ps(void *buf, long len)
{
    long max = len / (long)sizeof(ps_entry_t);
    long n = 0;
    ps_entry_t *out = (ps_entry_t *)buf;
    for (int i = 0; i < g_nthreads && n < max; i++) {
        out[n].id = g_threads[i].id;
        memset(out[n].name, 0, sizeof(out[n].name));
        strncpy(out[n].name, g_threads[i].name, sizeof(out[n].name) - 1);
        out[n].state = (int32_t)g_threads[i].state;
        n++;
    }
    return n * (long)sizeof(ps_entry_t);
}

int proc_fd_alloc(int vnode)
{
    thread_t *t = g_current;
    if (!t)
        return -1;
    for (int i = 3; i < PROC_MAX_FDS; i++)
        if (t->fds[i].vnode < 0) {
            t->fds[i].vnode = vnode;
            t->fds[i].off = 0;
            return i;
        }
    return -1;
}

int proc_fd_close(int fd)
{
    thread_t *t = g_current;
    if (!t || fd < 3 || fd >= PROC_MAX_FDS || t->fds[fd].vnode < 0)
        return -1;
    t->fds[fd].vnode = -1;
    t->fds[fd].off = 0;
    return 0;
}

int proc_fd_vnode(int fd)
{
    thread_t *t = g_current;
    if (!t || fd < 0 || fd >= PROC_MAX_FDS)
        return -1;
    return t->fds[fd].vnode;
}

uint32_t *proc_fd_offset(int fd)
{
    thread_t *t = g_current;
    if (!t || fd < 0 || fd >= PROC_MAX_FDS || t->fds[fd].vnode < 0)
        return 0;
    return &t->fds[fd].off;
}

void proc_exit_current(void)
{
    /* Runs on the process's kernel stack inside the syscall. Mark it DEAD
     * and park: the next timer IRQ switches to someone else for good. */
    cli();
    if (g_current)
        g_current->state = TH_DEAD;
    g_force_switch = 1;
    sti();
    for (;;)
        hlt();
}
