/* SMP bring-up (Phase 8c) — see smp.h. */
#include "smp.h"
#include "gdt.h"
#include "idt.h"
#include "cpu.h"
#include "lib/uart.h"
#include "drivers/lapic.h"
#include "drivers/timer.h"
#include "proc/syscall.h"

percpu_t g_percpu[MAX_CPUS];
uint64_t g_scratch_arr[MAX_CPUS];
uint64_t g_kstack_arr[MAX_CPUS];
volatile int g_cpus_online;
volatile int g_smp_total = 1;
volatile int g_sched_ready;

/* Per-CPU idle stacks (the scheduler redirects a dead CPU's IRQ frame to
 * smp_idle_trampoline running on one of these). */
static uint8_t g_idle_stack[MAX_CPUS][8192] __attribute__((aligned(16)));

void smp_idle_trampoline(void)
{
    for (;;) {
        sti();
        hlt();      /* wakes on the local APIC timer IRQ, which may switch */
    }
}

uint64_t smp_idle_rsp(int cpu)
{
    if (cpu < 0 || cpu >= MAX_CPUS)
        cpu = 0;
    return (uint64_t)g_idle_stack[cpu] + sizeof(g_idle_stack[cpu]);
}

void smp_bsp_init(struct limine_mp_request *req)
{
    /* BSP per-CPU state via GS. Set BOTH MSRs to the percpu pointer so the
     * SYSCALL/SYSRET GS swap is an identity — a thread that migrates CPUs
     * mid-syscall (yield + resume elsewhere) can't corrupt the pairing. */
    wrmsr(MSR_GS_BASE, (uint64_t)&g_percpu[0]);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)&g_percpu[0]);
    g_percpu[0].cpu = 0;
    g_percpu[0].online = 1;
    g_cpus_online = 1;
    /* CRITICAL (8c/7d fix): set the SYSCALL MSRs (EFER.SCE/STAR/LSTAR/SFMASK)
     * HERE, on the boot vCPU, BEFORE releasing the APs. smp_bsp_init() runs
     * before any AP timer exists, so the main thread is guaranteed to still
     * be on the boot vCPU — calling syscall_init_cpu() later from kmain
     * (syscall_init) is racy: by then the scheduler may have migrated main
     * to an AP, leaving the boot vCPU (apicid 0) without SCE, and the first
     * user `syscall`/`sysretq` on it #UDs (observed sporadically). APs set
     * their own in ap_entry(). */
    syscall_init_cpu();

    if (!req || !req->response) {
        uart_printf("smp: no Limine MP response, running single-CPU\n");
        g_smp_total = 1;
        return;
    }
    struct limine_mp_response *r = req->response;
    g_smp_total = (int)r->cpu_count;
    int aps = 0;
    for (uint64_t i = 0; i < r->cpu_count; i++) {
        struct limine_mp_info *info = r->cpus[i];
        if (info->lapic_id == r->bsp_lapic_id)
            continue;
        if (info->lapic_id >= MAX_CPUS)
            continue;
        info->goto_address = ap_entry;    /* Limine releases the AP now */
        aps++;
    }
    uart_printf("smp: Limine reports %llu cpus (bsp lapic %u), started %d APs\n",
                (unsigned long long)r->cpu_count, r->bsp_lapic_id, aps);
}

void smp_wait_online(void)
{
    /* Bounded wait (~3 s of TSC) for all APs to mark themselves online. */
    uint64_t t0 = rdtsc();
    uint64_t hz = timer_tsc_hz();
    while (g_cpus_online < g_smp_total && rdtsc() - t0 < hz * 3)
        __asm__ volatile ("pause");
    uart_printf("smp: %d cpus online\n", g_cpus_online);
}

void ap_entry(struct limine_mp_info *info)
{
    cli();
    int cpu = (int)info->lapic_id;
    if (cpu < 0 || cpu >= MAX_CPUS)
        cpu = 0;

    /* Per-CPU GDT/TSS (lgdt/ltr are per-CPU). gdt_flush no longer reloads gs,
     * so the GS base we set below survives. */
    g_percpu[cpu].cpu = cpu;
    gdt_init_cpu(cpu);

    /* Per-CPU GS base — set AFTER gdt_init_cpu, and BOTH MSRs so the
     * SYSCALL/SYSRET GS swap is an identity (a thread migrating CPUs
     * mid-syscall can't corrupt the pairing). */
    wrmsr(MSR_GS_BASE, (uint64_t)&g_percpu[cpu]);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)&g_percpu[cpu]);

    /* SYSCALL/SYSRET MSRs are per-CPU and syscall_init() only runs on the
     * BSP — without this, an AP's `syscall` jumps to LSTAR=0 (observed as
     * #PF at rip=0 cs=0 right after the first ring-3 syscall). */
    syscall_init_cpu();

    /* Wait for the BSP to finish IDT / timer / scheduler init (spin with
     * IF=0 — hlt would never wake). */
    while (!g_idt_ready)
        __asm__ volatile ("pause");
    idt_load();
    lapic_init();
    while (!g_timer_ready)
        __asm__ volatile ("pause");
    while (!g_sched_ready)
        __asm__ volatile ("pause");

    /* Start this CPU's local APIC timer so the scheduler runs here too. */
    lapic_timer_periodic(TIMER_IRQ_VECTOR, (uint32_t)(timer_lapic_hz() / TIMER_HZ));

    percpu_t *c = this_cpu();
    c->current = 0;          /* idle until the scheduler picks a thread */
    c->ticks = 0;
    c->force_switch = 0;
    c->online = 1;
    __sync_fetch_and_add(&g_cpus_online, 1);

    uart_printf("smp: AP lapic=%d online\n", cpu);

    smp_idle_trampoline();   /* never returns */
}
