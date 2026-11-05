/* MirixOS first user program (ring 3). Freestanding — no libc yet. */

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

static long sys_write(long fd, const void *buf, long len)
{
    return syscall3(SYS_write, fd, (long)buf, len);
}

static long sys_gettid(void)
{
    return syscall3(SYS_gettid, 0, 0, 0);
}

static void puts(const char *s)
{
    long n = 0;
    while (s[n])
        n++;
    sys_write(1, s, n);
}

long main(void)
{
    puts("hello from ring 3!\n");
    long r = sys_write(1, "user: write returned 19? checking...\n", 36);
    /* report the write() return value as a single hex-ish line */
    char buf[64];
    buf[0] = 'r'; buf[1] = 'e'; buf[2] = 't'; buf[3] = '=';
    long v = r;
    for (int i = 0; i < 8; i++) {
        int d = (int)(v & 0xf);
        buf[4 + (7 - i)] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    }
    buf[12] = '\n';
    sys_write(1, buf, 13);

    long tid = sys_gettid();
    char t[16];
    t[0] = 't'; t[1] = 'i'; t[2] = 'd'; t[3] = '=';
    t[4] = (char)('0' + (tid & 0xf));
    t[5] = '\n';
    sys_write(1, t, 6);

    return 42;   /* -> SYS_exit(42) from start.asm */
}
