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
int  process_spawn_user(const char *name, const uint8_t *elf, size_t len);
/* Phase 7b: like process_spawn_user, but fds 0/1/2 are bound to the given
 * vnodes (e.g. the slave side of a pty) instead of the console. */
int  process_spawn_user_fd(const char *name, const uint8_t *elf, size_t len,
                           int fd0_vnode, int fd1_vnode); /* ring 3 */
void sched_tick(registers_t *frame);         /* called from the timer IRQ */
void sched_switch_from_irq(registers_t *frame);
void sched_yield(void);                      /* cooperative reschedule (syscall) */
void proc_exit_current(void);                /* mark running process DEAD + park */
uint64_t sched_switches(void);
const char *current_thread_name(void);
int current_thread_id(void);

/* PML4 physical address of the process running on THIS CPU (SMP, 8c:
 * per-CPU state lives in percpu_t via KERNEL_GS_BASE; syscall.asm reads the
 * current kernel stack top from [gs:8]). */
uint64_t current_pml4_phys(void);

/* Per-process file descriptor table (fds 0/1/2 = console vnode 0). */
#define PROC_MAX_FDS 16
int       proc_fd_alloc(int vnode);     /* returns fd or -1 */
int       proc_fd_close(int fd);
int       proc_fd_vnode(int fd);        /* vnode index or -1 */
uint32_t *proc_fd_offset(int fd);       /* &fd->offset, or NULL */

/* ps(2)-style process listing — 32-byte entries shared with userland */
typedef struct {
    int32_t id;
    char    name[24];
    int32_t state;      /* 0=RUNNING 1=READY 2=DEAD */
} ps_entry_t;

long sched_ps(void *buf, long len);     /* returns bytes written */

#endif
