#include "thread.h"
#include "mm/mm.h"
#include "mm/kmalloc.h"
#include "mm/vmm.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/smp.h"
#include "drivers/timer.h"
#include "proc/elf.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "lib/spinlock.h"

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
static volatile uint64_t g_switches;

/* SMP (8c): the running thread is PER-CPU — this_cpu()->current (NULL = the
 * CPU is parked in the idle trampoline). All thread-table mutations happen
 * under g_sched_lock. Lock order: sched -> {vfs, wm, heap, pmm, uart}. */
static spinlock_t g_sched_lock = SPINLOCK_INIT;

static percpu_t *cur_cpu(void)
{
    percpu_t *c = this_cpu();
    return c ? c : &g_percpu[0];     /* before smp_bsp_init: BSP slot */
}

static thread_t *cur_thread(void)
{
    return (thread_t *)cur_cpu()->current;
}

const char *current_thread_name(void)
{
    thread_t *t = cur_thread();
    return t ? t->name : "?";
}

int current_thread_id(void)
{
    thread_t *t = cur_thread();
    return t ? t->id : -1;
}

uint64_t current_pml4_phys(void)
{
    thread_t *t = cur_thread();
    return t ? t->pml4_phys : 0;
}

uint64_t sched_switches(void) { return g_switches; }

/* Entry point for brand-new ring-0 threads: runs on their fresh stack. */
static void thread_trampoline(void)
{
    thread_t *me = cur_thread();
    thread_fn_t fn = me->entry;
    fn();
    uint64_t fl = spin_lock_irqsave(&g_sched_lock);
    me->state = TH_DEAD;
    cur_cpu()->force_switch = 1;     /* make this CPU reschedule immediately */
    spin_unlock_irqrestore(&g_sched_lock, fl);
    uart_printf("[%s] exited\n", me->name);
    for (;;) {
        sti();
        hlt();                        /* until the timer IRQ parks us idle */
    }
}

/* Caller must hold g_sched_lock (IF=0). */
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
    uint64_t fl = spin_lock_irqsave(&g_sched_lock);
    thread_t *t = alloc_slot(name);
    if (!t) {
        spin_unlock_irqrestore(&g_sched_lock, fl);
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
    spin_unlock_irqrestore(&g_sched_lock, fl);
    return t->id;
}

int process_spawn_user(const char *name, const uint8_t *elf, size_t len)
{
    return process_spawn_user_fd(name, elf, len, 0, 0);   /* console fds */
}

int process_spawn_user_fd(const char *name, const uint8_t *elf, size_t len,
                          int fd0_vnode, int fd1_vnode)
{
    uint64_t fl = spin_lock_irqsave(&g_sched_lock);
    thread_t *t = alloc_slot(name);
    if (!t) {
        spin_unlock_irqrestore(&g_sched_lock, fl);
        return -1;
    }
    /* bind fds 0/1/2 to the requested vnodes (alloc_slot's fds_init gave
     * them the console); offsets start at 0 */
    t->fds[0].vnode = fd0_vnode; t->fds[0].off = 0;
    t->fds[1].vnode = fd1_vnode; t->fds[1].off = 0;
    t->fds[2].vnode = fd1_vnode; t->fds[2].off = 0;
    t->is_user = 1;
    t->state = TH_READY;

    /* Private address space; load the ELF into it. */
    t->pml4_phys = vmm_create_space();
    uint64_t prev = read_cr3() & PTE_ADDR_MASK;
    write_cr3(t->pml4_phys);
    uint64_t entry = elf_load(elf, len);
    write_cr3(prev);
    if (entry == 0) {
        spin_unlock_irqrestore(&g_sched_lock, fl);
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
    spin_unlock_irqrestore(&g_sched_lock, fl);
    return t->id;
}

void sched_init(void)
{
    memset(g_threads, 0, sizeof(g_threads));
    g_nthreads = 1;
    percpu_t *c = cur_cpu();
    thread_t *m = &g_threads[0];
    c->current = m;
    m->id = 0;
    strcpy(m->name, "main");
    m->state = TH_RUNNING;
    m->stack_base = 0;        /* boot stack */
    uint64_t sp;
    m->kstack_top = (uint64_t)&sp & ~15ULL;
    m->pml4_phys = read_cr3() & PTE_ADDR_MASK;
    m->is_user = 0;
    fds_init(m);
    c->kstack_top = m->kstack_top;
    g_kstack_arr[c->cpu] = m->kstack_top;  /* syscall.asm reads this per-CPU */
    tss_set_rsp0(m->kstack_top);
    g_switches = 0;
    g_sched_ready = 1;    /* SMP: APs parked in ap_entry may now start timers */
    uart_printf("sched: round-robin ready (%d procs, quantum %d ticks)\n",
                g_nthreads, SCHED_QUANTUM_TICKS);
}

/* Caller must hold g_sched_lock and run with IF=0 (timer IRQ path). */
static void sched_switch_locked(registers_t *frame)
{
    percpu_t *c = cur_cpu();
    thread_t *cur = (thread_t *)c->current;

    if (cur) {
        /* Save the current thread's full state (force IF on for the resume). */
        cur->saved = *frame;
        cur->saved.rflags |= 0x200;
        if (cur->state == TH_RUNNING)
            cur->state = TH_READY;
    }

    /* Round-robin: first READY thread after cur (from slot 0 if idle). */
    thread_t *next = 0;
    int start = cur ? cur->id + 1 : 0;
    for (int i = 0; i < g_nthreads; i++) {
        thread_t *t = &g_threads[(start + i) % g_nthreads];
        if (t->state == TH_READY) {
            next = t;
            break;
        }
    }

    if (!next) {
        if (!cur)
            return;                       /* already idle: keep the idle frame */
        if (cur->state == TH_DEAD) {
            /* Nobody runnable and the current thread is dead: park this CPU
             * in the idle trampoline (its kernel stack is abandoned). */
            frame->rip = (uint64_t)smp_idle_trampoline;
            frame->rsp = smp_idle_rsp(c->cpu);
            frame->cs = SEL_KCODE;
            frame->ss = SEL_KDATA;
            frame->rflags = 0x202;
            c->current = 0;
            c->kstack_top = 0;
            g_kstack_arr[c->cpu] = 0;
        }
        return;                           /* otherwise keep cur running */
    }

    /* Cross address space + kernel stack if needed (IF=0: atomic). */
    if (!cur || next->pml4_phys != cur->pml4_phys)
        write_cr3(next->pml4_phys);
    tss_set_rsp0(next->kstack_top);
    c->kstack_top = next->kstack_top;
    g_kstack_arr[c->cpu] = next->kstack_top;

    next->state = TH_RUNNING;
    *frame = next->saved;                 /* iretq will now resume `next` */
    c->current = next;
    g_switches++;
}

void sched_switch_from_irq(registers_t *frame)
{
    spin_lock(&g_sched_lock);            /* IRQ context: IF already 0 */
    sched_switch_locked(frame);
    spin_unlock(&g_sched_lock);
}

void sched_tick(registers_t *frame)
{
    if (!g_sched_ready)
        return;
    percpu_t *c = cur_cpu();
    if (c->force_switch || c->ticks % SCHED_QUANTUM_TICKS == 0) {
        c->force_switch = 0;
        spin_lock(&g_sched_lock);
        sched_switch_locked(frame);
        spin_unlock(&g_sched_lock);
    }
}

void sched_yield(void)
{
    /* Called from a syscall (IF=0). Wait for the next timer IRQ, which will
     * switch to another thread; on resume we continue right after hlt. */
    percpu_t *c = cur_cpu();
    c->force_switch = 1;
    sti();
    hlt();
    cli();
    c->force_switch = 0;
}

long sched_ps(void *buf, long len)
{
    long max = len / (long)sizeof(ps_entry_t);
    long n = 0;
    ps_entry_t *out = (ps_entry_t *)buf;
    spin_lock(&g_sched_lock);            /* syscall context: IF=0 */
    for (int i = 0; i < g_nthreads && n < max; i++) {
        out[n].id = g_threads[i].id;
        memset(out[n].name, 0, sizeof(out[n].name));
        strncpy(out[n].name, g_threads[i].name, sizeof(out[n].name) - 1);
        out[n].state = (int32_t)g_threads[i].state;
        n++;
    }
    spin_unlock(&g_sched_lock);
    return n * (long)sizeof(ps_entry_t);
}

int proc_fd_alloc(int vnode)
{
    thread_t *t = cur_thread();
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
    thread_t *t = cur_thread();
    if (!t || fd < 3 || fd >= PROC_MAX_FDS || t->fds[fd].vnode < 0)
        return -1;
    t->fds[fd].vnode = -1;
    t->fds[fd].off = 0;
    return 0;
}

int proc_fd_vnode(int fd)
{
    thread_t *t = cur_thread();
    if (!t || fd < 0 || fd >= PROC_MAX_FDS)
        return -1;
    return t->fds[fd].vnode;
}

uint32_t *proc_fd_offset(int fd)
{
    thread_t *t = cur_thread();
    if (!t || fd < 0 || fd >= PROC_MAX_FDS || t->fds[fd].vnode < 0)
        return 0;
    return &t->fds[fd].off;
}

void proc_exit_current(void)
{
    /* Runs on the process's kernel stack inside the syscall. Mark it DEAD
     * and park: the next timer IRQ switches to someone else for good. */
    uint64_t fl = spin_lock_irqsave(&g_sched_lock);
    thread_t *me = cur_thread();
    if (me)
        me->state = TH_DEAD;
    cur_cpu()->force_switch = 1;
    spin_unlock_irqrestore(&g_sched_lock, fl);
    for (;;) {
        sti();
        hlt();
    }
}
