/* Timer: TSC calibrated against the PIT, then driven by the local APIC
 * timer. TIMER_IRQ_VECTOR is the IDT vector the LAPIC timer uses. */
#ifndef MIRIX_DRIVERS_TIMER_H
#define MIRIX_DRIVERS_TIMER_H

#include <stdint.h>

#define TIMER_IRQ_VECTOR 0x30   /* 48: above the 16 PIC IRQs (32-47) */
#define TIMER_HZ         100

void timer_init(void);          /* calibrate TSC via PIT, start LAPIC timer */
void timer_irq(void);           /* called from idt.c on TIMER_IRQ_VECTOR */
uint64_t timer_ticks(void);
uint64_t timer_tsc_hz(void);
void msleep(uint64_t ms);       /* busy-wait using the calibrated TSC */

#endif
