#include "syscall.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "lib/uart.h"
#include "lib/panic.h"
#include "proc/thread.h"

#define MSR_EFER    0xc0000080
#define MSR_STAR    0xc0000081
#define MSR_LSTAR   0xc0000082
#define MSR_SFMASK  0xc0000084
#define EFER_SCE    (1ULL << 0)

extern void syscall_entry(void);

/* User addresses must live below the half-way canonical boundary. */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

static int is_user_addr(uint64_t p) { return p < USER_ADDR_LIMIT; }

void syscall_init(void)
{
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    /* STAR: [47:32] = kernel CS (syscall: CS=0x08, SS=0x10),
     *       [63:48] = user base 0x10 (sysret: SS=0x18=udata, CS=0x20=ucode) */
    wrmsr(MSR_STAR, ((uint64_t)SEL_KCODE << 32) | ((uint64_t)0x10 << 48));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x200);   /* clear IF on syscall entry */
    /* TSS rsp0 is set per-process by the scheduler (sched_switch_from_irq). */

    uart_printf("syscall: STAR=%llx LSTAR=%p SFMASK=0x200\n",
                (unsigned long long)rdmsr(MSR_STAR), (void *)(uint64_t)syscall_entry);
}

uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6,
                          uint64_t user_rip, uint64_t user_rflags, uint64_t user_rsp)
{
    (void)a4; (void)a5; (void)a6;
    (void)user_rip; (void)user_rflags; (void)user_rsp;

    switch (nr) {
    case SYS_write: {
        uint64_t fd = a1, buf = a2, len = a3;
        if (fd != 1 && fd != 2)
            return (uint64_t)-1;                 /* EBADF-ish */
        if (!is_user_addr(buf) || !is_user_addr(buf + len))
            return (uint64_t)-2;                 /* EFAULT-ish: no kernel pointers */
        uart_write((const char *)buf, len);
        return len;
    }
    case SYS_gettid:
        return (uint64_t)current_thread_id();
    case SYS_yield:
        sched_yield();
        return 0;
    case SYS_exit: {
        /* Park the process: mark DEAD, then wait for the timer IRQ to
         * switch away (the scheduler never reselects DEAD processes). */
        uart_printf("[user %d] exit(%llu)\n", current_thread_id(),
                    (unsigned long long)a1);
        proc_exit_current();
        return 0;   /* not reached */
    }
    default:
        uart_printf("syscall: unknown nr %llu from rip=%p\n",
                    (unsigned long long)nr, (void *)user_rip);
        return (uint64_t)-38;                    /* ENOSYS-ish */
    }
}
