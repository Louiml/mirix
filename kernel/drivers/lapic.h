/* Local APIC (xAPIC mode). Registers are memory-mapped at physical
 * 0xFEE00000, reachable through the HHDM. Used for the timer IRQ. */
#ifndef MIRIX_DRIVERS_LAPIC_H
#define MIRIX_DRIVERS_LAPIC_H

#include <stdint.h>

#define LAPIC_PHYS_BASE 0xFEE00000ULL

/* Register offsets */
#define LAPIC_EOI        0x0b0
#define LAPIC_SVR        0x0f0   /* spurious interrupt vector */
#define LAPIC_TIMER_DIV  0x3e0
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR  0x390
#define LAPIC_TIMER_LVT  0x320

#define LAPIC_SVR_ENABLE (1u << 8)

uint32_t lapic_read(uint32_t offset);
void lapic_write(uint32_t offset, uint32_t value);
void lapic_init(void);                        /* software-enable xAPIC + SVR */
/* LVT timer, divide-by-1. The APIC bus frequency is unknown (QEMU TCG
 * assumes 1 GHz, KVM uses the TSC), so timer.c calibrates it at runtime
 * with a one-shot count measured against the PIT-calibrated TSC. */
void lapic_timer_oneshot(uint8_t vector, uint32_t count);
void lapic_timer_periodic(uint8_t vector, uint32_t count);
void lapic_eoi(void);

#endif
