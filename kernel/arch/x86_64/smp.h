/* SMP support (Phase 8c). Per-CPU state lives in g_percpu[], addressed via
 * the KERNEL_GS_BASE MSR (swapped in by SYSCALL, so [gs:x] in ring 0 is the
 * running CPU's percpu block; syscall.asm uses gs+0 for the user-rsp stash
 * and gs+8 for the current thread's kernel stack top).
 *
 * APs are started by Limine's MP request: the BSP sets each AP's
 * goto_address to ap_entry(); each AP builds its own GDT/TSS, loads the
 * shared IDT, enables its local APIC + timer, marks itself online and
 * parks in the per-CPU idle loop — from then on its timer IRQ runs the
 * normal scheduler. */
#ifndef MIRIX_ARCH_SMP_H
#define MIRIX_ARCH_SMP_H

#include <stdint.h>
#include "cpu.h"
#include "limine.h"

#define MAX_CPUS            8
#define MSR_GS_BASE         0xC0000100
#define MSR_KERNEL_GS_BASE  0xC0000102

typedef struct percpu {
    uint64_t scratch;       /* gs+0: syscall_entry user-rsp stash */
    uint64_t kstack_top;    /* gs+8: current thread's kernel stack top */
    int      cpu;           /* slot index (= lapic id) */
    int      online;
    uint64_t ticks;         /* per-CPU timer ticks (scheduler quantum) */
    int      force_switch;  /* per-CPU yield request */
    void    *current;       /* thread_t* running on this CPU, NULL = idle */
} percpu_t;

extern percpu_t g_percpu[MAX_CPUS];
extern volatile int g_cpus_online;   /* APs that finished init */
extern volatile int g_smp_total;     /* total CPUs incl. BSP (Limine) */
extern volatile int g_sched_ready;   /* set at the end of sched_init() */

/* Flat per-CPU arrays used by syscall.asm (which cannot rely on the GS swap:
 * it reads the local APIC id from MMIO and indexes these). Kept in sync with
 * g_percpu[] by the scheduler. */
extern uint64_t g_scratch_arr[MAX_CPUS];   /* user rsp stash */
extern uint64_t g_kstack_arr[MAX_CPUS];    /* current thread kstack top */

static inline percpu_t *this_cpu(void)
{
    return (percpu_t *)rdmsr(MSR_KERNEL_GS_BASE);
}

/* BSP: point KERNEL_GS_BASE at g_percpu[0] and start all APs (Limine MP). */
void smp_bsp_init(struct limine_mp_request *req);
/* BSP: wait (bounded) for all APs to come online; prints the smp marker. */
void smp_wait_online(void);

/* AP entry point (Limine goto_address). Never returns. */
void ap_entry(struct limine_mp_info *info);

/* Idle loop used when a CPU has no runnable thread (scheduler jumps here
 * by rewriting the IRQ frame). */
void smp_idle_trampoline(void);
uint64_t smp_idle_rsp(int cpu);

#endif
