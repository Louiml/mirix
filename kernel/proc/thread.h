/* Processes & scheduler. Each thread/process owns:
 *   - a saved registers_t frame (how the IRQ-path switch resumes it)
 *   - a PML4 (address space; kernel half shared, user half private)
 *   - a kernel stack (TSS rsp0 + syscall-entry stack while it runs)
 * Kernel threads run ring 0 with cs=0x08; user processes get a fabricated
 * ring-3 frame (cs=0x23, ss=0x1B) and are entered via the same iretq path.
 *
 * Switching happens inside the timer IRQ handler: the ISR stub pushed a
 * full registers_t frame on the current stack, so sched_switch_from_irq()
 * saves that frame into the current process and copies the next process's
 * saved frame over it; the normal iretq epilogue then "becomes" the next
 * process. CR3 and rsp0 are swapped in the same critical section (IF=0).
 */
#ifndef MIRIX_PROC_THREAD_H
#define MIRIX_PROC_THREAD_H

#include <stdint.h>
#include <stddef.h>
#include "arch/x86_64/idt.h"   /* registers_t */

typedef void (*thread_fn_t)(void);

void sched_init(void);                       /* adopts the boot stack as "main" */
int  thread_create(const char *name, thread_fn_t fn);            /* ring 0 */
int  process_spawn_user(const char *name, const uint8_t *elf, size_t len); /* ring 3 */
void sched_tick(registers_t *frame);         /* called from the timer IRQ */
void sched_switch_from_irq(registers_t *frame);
void sched_yield(void);                      /* cooperative reschedule (syscall) */
void proc_exit_current(void);                /* mark running process DEAD + park */
uint64_t sched_switches(void);
const char *current_thread_name(void);
int current_thread_id(void);

/* Top of the running process's kernel stack — syscall_entry switches to it. */
extern uint64_t g_current_kstack_top;

#endif
