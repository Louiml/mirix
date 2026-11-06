/* Spinlocks for SMP (Phase 8c). All kernel locks use the irqsave variants:
 * the holder runs with IF=0 while holding, so an interrupt on the same CPU
 * can never deadlock against itself. Lock ordering (no cycles):
 *   sched -> vfs/wm -> heap -> pmm ;  uart / input / net are leaves. */
#ifndef MIRIX_LIB_SPINLOCK_H
#define MIRIX_LIB_SPINLOCK_H

#include <stdint.h>

typedef struct {
    volatile int locked;
} spinlock_t;

#define SPINLOCK_INIT { 0 }

/* Returns the saved rflags (so the caller can restore IF). */
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags;
    __asm__ volatile ("pushfq\n\tpop %0\n\tcli" : "=r"(flags) : : "memory");
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked)
            __asm__ volatile ("pause");
    }
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    __sync_lock_release(&l->locked);
    if (flags & 0x200ULL)
        __asm__ volatile ("sti");
}

/* Plain variants for contexts that already run with IF=0 (IRQ handlers). */
static inline void spin_lock(spinlock_t *l)
{
    while (__sync_lock_test_and_set(&l->locked, 1)) {
        while (l->locked)
            __asm__ volatile ("pause");
    }
}

static inline void spin_unlock(spinlock_t *l)
{
    __sync_lock_release(&l->locked);
}

#endif
