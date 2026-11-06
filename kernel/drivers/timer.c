#include "timer.h"
#include "pit.h"
#include "lapic.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/smp.h"
#include "lib/uart.h"

static volatile uint64_t g_ticks;    /* SMP: total ticks across all CPUs */
static uint64_t g_tsc_hz;
static uint64_t g_lapic_hz;
volatile int g_timer_ready;

/* Calibrate the TSC: run PIT channel 0 one-shot for a known number of
 * 1.193182 MHz ticks and measure how many TSC ticks elapse. */
static uint64_t calibrate_tsc(void)
{
    const uint32_t divisor = 0;            /* 0 = 65536 counts ~= 54.9 ms */
    pit_oneshot((uint16_t)divisor);

    /* Phase 1: wait until the new count is loaded and decrementing — the
     * first latch reads can return a stale 0xffff from before the reload. */
    for (uint64_t guard = 0; guard < 100000000ULL; guard++) {
        if (pit_read_count() < 0xf000)
            break;
    }
    uint64_t t0 = rdtsc();
    /* Phase 2: poll until terminal count (0), or a wrap past 0xf000
     * (mode 0 stops at 0 on real hw; QEMU may wrap to 0xffff). */
    for (uint64_t guard = 0; guard < 100000000ULL; guard++) {
        uint16_t c = pit_read_count();
        if (c == 0 || c > 0xf000)
            break;
    }
    uint64_t t1 = rdtsc();

    uint64_t pit_ticks = (divisor == 0) ? 65536 : divisor;
    return (t1 - t0) * PIT_FREQ_HZ / pit_ticks;
}

/* The local APIC timer counts at the "APIC bus" rate, which differs between
 * QEMU TCG (~1 GHz fixed) and KVM (guest TSC). Calibrate it: fire a one-shot
 * of CALIB_COUNT ticks and measure the wall time with the calibrated TSC. */
#define CALIB_COUNT 50000000ULL   /* 50 ms if the bus runs at 1 GHz */

static uint64_t calibrate_lapic(void)
{
    g_ticks = 0;
    uint64_t t0 = rdtsc();
    lapic_timer_oneshot(TIMER_IRQ_VECTOR, (uint32_t)CALIB_COUNT);
    sti();
    uint64_t guard = rdtsc();
    while (g_ticks == 0 && rdtsc() - guard < g_tsc_hz)   /* max ~1 s */
        hlt();
    cli();
    uint64_t t1 = rdtsc();
    if (g_ticks == 0) {
        uart_printf("timer: lapic calibration timed out, assuming TSC rate\n");
        return g_tsc_hz;
    }
    return CALIB_COUNT * g_tsc_hz / (t1 - t0);
}

void timer_init(void)
{
    g_tsc_hz = calibrate_tsc();
    uart_printf("timer: tsc calibrated: %llu Hz (%llu MHz) via PIT\n",
                (unsigned long long)g_tsc_hz, (unsigned long long)(g_tsc_hz >> 20));

    lapic_init();
    uint64_t lapic_hz = calibrate_lapic();
    uart_printf("timer: lapic bus calibrated: %llu Hz\n", (unsigned long long)lapic_hz);

    lapic_timer_periodic(TIMER_IRQ_VECTOR, (uint32_t)(lapic_hz / TIMER_HZ));
    uart_printf("lapic: timer started (periodic), vector 0x%x, %d Hz (%llu ticks/period)\n",
                TIMER_IRQ_VECTOR, TIMER_HZ, (unsigned long long)(lapic_hz / TIMER_HZ));
    g_ticks = 0;
    g_lapic_hz = lapic_hz;
    g_timer_ready = 1;      /* SMP: APs wait for this before starting theirs */
}

void timer_irq(void)
{
    __sync_fetch_and_add(&g_ticks, 1);       /* SMP: global counter, atomic */
    percpu_t *c = this_cpu();
    if (c)
        c->ticks++;                         /* per-CPU quantum counter */
    lapic_eoi();
}

uint64_t timer_ticks(void) { return g_ticks; }
uint64_t timer_tsc_hz(void) { return g_tsc_hz; }
uint64_t timer_lapic_hz(void) { return g_lapic_hz; }

void msleep(uint64_t ms)
{
    uint64_t start = rdtsc();
    uint64_t delta = g_tsc_hz / 1000 * ms;
    /* Spin (pause) rather than hlt: the wait itself must not depend on an
     * IRQ arriving. Timer IRQs still fire and are counted meanwhile. */
    while (rdtsc() - start < delta)
        __asm__ volatile ("pause");
}
