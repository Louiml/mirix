/* Kernel threads + round-robin scheduler.
 *
 * Switching happens inside the timer IRQ handler: the ISR stub has already
 * pushed a full registers_t frame on the current thread's stack, so
 * sched_switch_from_irq() just saves that frame into the current thread and
 * copies the next thread's saved frame over it. The normal iretq epilogue
 * then pops the next thread's rip/cs/rflags/rsp/ss and "becomes" it.
 * Interrupt gates clear IF, so the whole switch is atomic.
 */
#ifndef MIRIX_PROC_THREAD_H
#define MIRIX_PROC_THREAD_H

#include <stdint.h>
#include "arch/x86_64/idt.h"   /* registers_t */

typedef void (*thread_fn_t)(void);

void sched_init(void);                       /* adopts the boot stack as "main" */
int  thread_create(const char *name, thread_fn_t fn);
void sched_tick(registers_t *frame);         /* called from the timer IRQ */
void sched_switch_from_irq(registers_t *frame);
uint64_t sched_switches(void);
const char *current_thread_name(void);

#endif
