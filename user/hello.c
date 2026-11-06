/* MirixOS user process (ring 3). Freestanding — no libc yet.
 * 1) prints P<tid>.<i> lines and yields (multi-process demo)
 * 2) exercises the VFS: create/write/close, open/read-back, list root dir
 */

static long syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write    0
#define SYS_exit     1
#define SYS_gettid   2
#define SYS_yield    3
#define SYS_open     4
#define SYS_read     5
#define SYS_close    6
#define SYS_getdents 7

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x80

static long sys_write(long fd, const void *buf, long len) { return syscall3(SYS_write, fd, (long)buf, len); }
static long sys_read(long fd, void *buf, long len)        { return syscall3(SYS_read, fd, (long)buf, len); }
static long sys_open(const char *p, long fl)              { return syscall3(SYS_open, (long)p, fl, 0); }
static long sys_close(long fd)                             { return syscall3(SYS_close, fd, 0, 0); }
static long sys_getdents(long fd, void *b, long n)         { return syscall3(SYS_getdents, fd, (long)b, n); }

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts(const char *s) { sys_write(1, s, slen(s)); }

/* append helpers — build each output line in one buffer so a single
 * write() syscall emits it atomically (no interleave with other processes) */
static long acpy(char *d, long n, const char *s) { long l = slen(s); for (long i = 0; i < l; i++) d[n + i] = s[i]; return n + l; }
static long adec(char *d, long n, long v)
{
    char t[20]; long m = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) d[n++] = '-';
    if (u == 0) t[m++] = '0';
    while (u) { t[m++] = (char)('0' + (u % 10)); u /= 10; }
    while (m) d[n++] = t[--m];
    return n;
}

/* dirent layout shared with the kernel — 40 bytes */
typedef struct { char name[32]; unsigned int size; unsigned char type; unsigned char pad[3]; } dirent_t;

long main(void)
{
    long tid = syscall3(SYS_gettid, 0, 0, 0);

    /* 1) multi-process demo */
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

    /* 2) VFS demo */
    char out[192];
    long on;

    const char *msg = "mirixfs says hi\n";
    long mlen = slen(msg);
    long fd = sys_open("/hello.txt", O_CREAT | O_WRONLY | O_TRUNC);
    long w = fd >= 0 ? sys_write(fd, msg, mlen) : -1;
    if (fd >= 0) sys_close(fd);
    on = 0;
    on = acpy(out, on, "fsdemo: wrote ");
    on = adec(out, on, w);
    on = acpy(out, on, " bytes to /hello.txt (fd was ");
    on = adec(out, on, fd);
    on = acpy(out, on, ")\n");
    sys_write(1, out, on);

    char rbuf[64];
    fd = sys_open("/hello.txt", O_RDONLY);
    long n = fd >= 0 ? sys_read(fd, rbuf, (long)sizeof rbuf) : -1;
    if (fd >= 0) sys_close(fd);
    on = 0;
    on = acpy(out, on, "fsdemo: read back: ");
    for (long i = 0; i < n && i < 60; i++)
        out[on++] = rbuf[i];
    if (n < 0) { out[on++] = '?'; }
    out[on++] = '\n';
    sys_write(1, out, on);

    fd = sys_open("/", O_RDONLY);
    char dbuf[512];
    long nb = fd >= 0 ? sys_getdents(fd, dbuf, (long)sizeof dbuf) : -1;
    if (fd >= 0) sys_close(fd);
    on = 0;
    on = acpy(out, on, "fsdemo: ls / (");
    on = adec(out, on, nb >= 0 ? nb / (long)sizeof(dirent_t) : -1);
    on = acpy(out, on, " entries):");
    if (nb > 0) {
        long cnt = nb / (long)sizeof(dirent_t);
        for (long i = 0; i < cnt; i++) {
            dirent_t *d = (dirent_t *)(dbuf + i * (long)sizeof(dirent_t));
            on = acpy(out, on, " ");
            on = acpy(out, on, d->name);
        }
    }
    on = acpy(out, on, "\n");
    sys_write(1, out, on);

    return 40 + (tid & 0xf);   /* exit code 43/44 -> SYS_exit via _start */
}
