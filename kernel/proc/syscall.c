#include "syscall.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/gdt.h"
#include "lib/uart.h"
#include "lib/panic.h"
#include "lib/string.h"
#include "proc/thread.h"
#include "fs/vfs.h"
#include "drivers/ps2.h"
#include "drivers/ahci.h"
#include "drivers/fb.h"
#include "mm/pmm.h"
#include "mm/mm.h"

#define MSR_EFER    0xc0000080
#define MSR_STAR    0xc0000081
#define MSR_LSTAR   0xc0000082
#define MSR_SFMASK  0xc0000084
#define EFER_SCE    (1ULL << 0)

extern void syscall_entry(void);

/* User addresses must live below the half-way canonical boundary. */
#define USER_ADDR_LIMIT 0x0000800000000000ULL

static int is_user_addr(uint64_t p) { return p < USER_ADDR_LIMIT; }

/* Copy a NUL-terminated string from user memory, bounded. The dispatch runs
 * on the caller's kernel stack under the caller's CR3, so user pages are
 * directly readable; we still range-check against the kernel half. */
static int user_strcpy(char *dst, uint64_t src, size_t max)
{
    if (!is_user_addr(src))
        return -1;
    for (size_t i = 0; i < max; i++) {
        if (!is_user_addr(src + i))
            return -1;
        char c = *(const char *)(src + i);
        dst[i] = c;
        if (c == 0)
            return 0;
    }
    dst[max - 1] = 0;
    return 0;
}

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
        if (!is_user_addr(buf) || !is_user_addr(buf + len))
            return (uint64_t)-2;                 /* EFAULT-ish: no kernel pointers */
        int vn = proc_fd_vnode((int)fd);
        if (vn < 0)
            return (uint64_t)-9;                 /* EBADF */
        if (vn == 0) {                           /* console */
            uart_write((const char *)buf, len);
            return len;
        }
        if (vfs_vnode_type(vn) != VN_FILE)
            return (uint64_t)-9;
        return (uint64_t)vfs_write(vn, proc_fd_offset((int)fd),
                                   (const void *)buf, (long)len);
    }
    case SYS_read: {
        uint64_t fd = a1, buf = a2, len = a3;
        if (!is_user_addr(buf) || !is_user_addr(buf + len))
            return (uint64_t)-2;
        int vn = proc_fd_vnode((int)fd);
        if (vn < 0)
            return (uint64_t)-9;
        if (vn == 0) {                           /* console: kbd ring, non-blocking */
            long n = 0;
            kbd_event_t ev;
            while (n < (long)len && kbd_pop(&ev))
                if (ev.pressed && ev.ascii)
                    ((char *)buf)[n++] = ev.ascii;
            return (uint64_t)n;
        }
        if (vfs_vnode_type(vn) != VN_FILE)
            return (uint64_t)-9;
        return (uint64_t)vfs_read(vn, proc_fd_offset((int)fd), (void *)buf, (long)len);
    }
    case SYS_open: {
        char path[VFS_NAME_MAX];
        if (user_strcpy(path, a1, sizeof(path)) != 0)
            return (uint64_t)-2;
        int vn = vfs_open(path, (int)a2);
        if (vn < 0)
            return (uint64_t)vn;
        int fd = proc_fd_alloc(vn);
        if (fd < 0)
            return (uint64_t)-24;                /* EMFILE */
        return (uint64_t)fd;
    }
    case SYS_close:
        return (uint64_t)proc_fd_close((int)a1);
    case SYS_getdents: {
        uint64_t fd = a1, buf = a2, len = a3;
        if (!is_user_addr(buf) || !is_user_addr(buf + len))
            return (uint64_t)-2;
        int vn = proc_fd_vnode((int)fd);
        if (vn < 0 || vfs_vnode_type(vn) != VN_DIR)
            return (uint64_t)-9;
        return (uint64_t)vfs_getdents(vn, (void *)buf, (long)len);
    }
    case SYS_ps: {
        uint64_t buf = a1, len = a2;
        if (!is_user_addr(buf) || !is_user_addr(buf + len))
            return (uint64_t)-2;
        return (uint64_t)sched_ps((void *)buf, (long)len);
    }
    case SYS_diskread: {
        uint64_t lba = a1, nsec = a2, buf = a3;
        if (nsec == 0 || nsec > AHCI_MAX_SECTORS_PER_READ)
            return (uint64_t)-22;               /* EINVAL */
        if (!is_user_addr(buf) || !is_user_addr(buf + nsec * 512))
            return (uint64_t)-2;
        if (!ahci_port_online())
            return (uint64_t)-19;               /* ENODEV */
        /* Bounce through a contiguous PMM block (the HBA needs phys addr). */
        uint64_t pages = (nsec * 512 + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t blk = pmm_alloc_pages_contig(pages);
        if (!blk)
            return (uint64_t)-12;
        long n = ahci_read(lba, (uint32_t)nsec, phys_to_virt(blk));
        if (n > 0)
            memcpy((void *)buf, phys_to_virt(blk), (size_t)n);
        for (uint64_t p = blk; p < blk + pages * PAGE_SIZE; p += PAGE_SIZE)
            pmm_free_page(p);
        return (uint64_t)n;
    }
    case SYS_flip:
        fb_present();
        return 0;
    case SYS_fbinfo: {
        uint64_t buf = a1;
        if (!is_user_addr(buf) || !is_user_addr(buf + 12))
            return (uint64_t)-2;
        uint32_t *out = (uint32_t *)buf;
        out[0] = fb_width();
        out[1] = fb_height();
        out[2] = 32;
        return 0;
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
