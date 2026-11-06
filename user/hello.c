/* MirixOS user process (ring 3). Freestanding — no libc yet.
 * Prints P<tid>.<i> lines and yields, so multiple processes interleave. */

static long syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write  0
#define SYS_exit   1
#define SYS_gettid 2
#define SYS_yield  3

static long sys_write(long fd, const void *buf, long len)
{
    return syscall3(SYS_write, fd, (long)buf, len);
}

long main(void)
{
    long tid = syscall3(SYS_gettid, 0, 0, 0);
    char line[8];
    for (int i = 0; i < 5; i++) {
        line[0] = 'P';
        line[1] = (char)('0' + (tid & 0xf));
        line[2] = '.';
        line[3] = (char)('0' + i);
        line[4] = '\n';
        sys_write(1, line, 5);
        syscall3(SYS_yield, 0, 0, 0);
    }
    return 40 + (tid & 0xf);   /* exit code 41/42... -> SYS_exit via _start */
}
