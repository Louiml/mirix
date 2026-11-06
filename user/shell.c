/* mirsh — MirixOS user shell (ring 3). Reads lines from fd 0 (keyboard),
 * echoes them, and runs: help ls cat <f> echo <text> write <f> <text> ps exit
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
#define SYS_ps       8

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0x40
#define O_TRUNC  0x80

static long sys_write(long fd, const void *buf, long len) { return syscall3(SYS_write, fd, (long)buf, len); }
static long sys_read(long fd, void *buf, long len)        { return syscall3(SYS_read, fd, (long)buf, len); }
static long sys_open(const char *p, long fl)              { return syscall3(SYS_open, (long)p, fl, 0); }
static long sys_close(long fd)                             { return syscall3(SYS_close, fd, 0, 0); }
static long sys_getdents(long fd, void *b, long n)         { return syscall3(SYS_getdents, fd, (long)b, n); }
static long sys_ps(void *b, long n)                        { return syscall3(SYS_ps, (long)b, n, 0); }
static long sys_yield(void)                                { return syscall3(SYS_yield, 0, 0, 0); }
static long sys_exit(long code)                            { return syscall3(SYS_exit, code, 0, 0); }

static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts(const char *s) { sys_write(1, s, slen(s)); }

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

typedef struct { char name[32]; unsigned int size; unsigned char type; unsigned char pad[3]; } dirent_t;
typedef struct { int id; char name[24]; int state; } ps_entry_t;

static int eq(const char *a, const char *b) { long i = 0; while (a[i] && a[i] == b[i]) i++; return a[i] == 0 && b[i] == 0; }

static void cmd_ls(void)
{
    long fd = sys_open("/", O_RDONLY);
    if (fd < 0) { puts("ls: cannot open /\n"); return; }
    char dbuf[512];
    long nb = sys_getdents(fd, dbuf, (long)sizeof dbuf);
    sys_close(fd);
    char out[256];
    long on = 0;
    on = acpy(out, on, "ls: ");
    on = adec(out, on, nb >= 0 ? nb / (long)sizeof(dirent_t) : -1);
    on = acpy(out, on, " entries\n");
    sys_write(1, out, on);
    if (nb > 0) {
        long cnt = nb / (long)sizeof(dirent_t);
        for (long i = 0; i < cnt; i++) {
            dirent_t *d = (dirent_t *)(dbuf + i * (long)sizeof(dirent_t));
            on = 0;
            on = acpy(out, on, "  ");
            on = acpy(out, on, d->name);
            on = acpy(out, on, " (");
            on = adec(out, on, (long)d->size);
            on = acpy(out, on, " bytes)\n");
            sys_write(1, out, on);
        }
    }
}

static void cmd_cat(const char *path)
{
    long fd = sys_open(path, O_RDONLY);
    if (fd < 0) { puts("cat: no such file\n"); return; }
    char buf[256];
    long n;
    while ((n = sys_read(fd, buf, (long)sizeof buf)) > 0)
        sys_write(1, buf, n);
    sys_close(fd);
    puts("\n");
}

static void cmd_write(const char *path, const char *text)
{
    long fd = sys_open(path, O_CREAT | O_WRONLY | O_TRUNC);
    if (fd < 0) { puts("write: cannot create\n"); return; }
    long w = sys_write(fd, text, slen(text));
    sys_close(fd);
    char out[96]; long on = 0;
    on = acpy(out, on, "wrote ");
    on = adec(out, on, w);
    on = acpy(out, on, " bytes to ");
    on = acpy(out, on, path);
    on = acpy(out, on, "\n");
    sys_write(1, out, on);
}

static void cmd_ps(void)
{
    ps_entry_t ents[16];
    long nb = sys_ps(ents, (long)sizeof ents);
    long cnt = nb / (long)sizeof(ps_entry_t);
    char out[128];
    for (long i = 0; i < cnt; i++) {
        long on = 0;
        on = acpy(out, on, "ps: id=");
        on = adec(out, on, ents[i].id);
        on = acpy(out, on, " name=");
        on = acpy(out, on, ents[i].name);
        on = acpy(out, on, " state=");
        on = acpy(out, on, ents[i].state == 0 ? "RUNNING" : ents[i].state == 1 ? "READY" : "DEAD");
        on = acpy(out, on, "\n");
        sys_write(1, out, on);
    }
}

long main(void)
{
    long tid = syscall3(SYS_gettid, 0, 0, 0);
    char out[64];
    long on = 0;
    on = acpy(out, on, "mirsh ready (pid ");
    on = adec(out, on, tid);
    on = acpy(out, on, ") — type 'help'\n");
    sys_write(1, out, on);

    char line[128];
    for (;;) {
        puts("mirix$ ");
        long ln = 0;
        for (;;) {
            char c[64];
            long n = sys_read(0, c, (long)sizeof c);
            if (n <= 0) { sys_yield(); continue; }
            for (long i = 0; i < n; i++) {
                char ch = c[i];
                if (ch == '\n') { sys_write(1, "\n", 1); goto exec; }
                if (ch == '\b') { if (ln > 0) { ln--; sys_write(1, "\b \b", 3); } continue; }
                if (ln < (long)sizeof line - 1) { line[ln++] = ch; sys_write(1, &ch, 1); }
            }
        }
exec:
        line[ln] = 0;
        /* split first word */
        long sp = 0;
        while (line[sp] && line[sp] != ' ') sp++;
        char cmd[32];
        long cl = sp < 31 ? sp : 31;
        for (long i = 0; i < cl; i++) cmd[i] = line[i];
        cmd[cl] = 0;
        const char *arg = line + sp;
        while (*arg == ' ') arg++;

        if (cmd[0] == 0) { /* empty */ }
        else if (eq(cmd, "help"))
            puts("commands: help | ls | cat <file> | echo <text> | write <file> <text> | ps | exit\n");
        else if (eq(cmd, "ls")) cmd_ls();
        else if (eq(cmd, "cat")) cmd_cat(arg);
        else if (eq(cmd, "echo")) { sys_write(1, arg, slen(arg)); sys_write(1, "\n", 1); }
        else if (eq(cmd, "write")) {
            /* write <file> <text...> */
            long s2 = 0;
            while (arg[s2] && arg[s2] != ' ') s2++;
            char path[32];
            long pl = s2 < 31 ? s2 : 31;
            for (long i = 0; i < pl; i++) path[i] = arg[i];
            path[pl] = 0;
            const char *text = arg + s2;
            while (*text == ' ') text++;
            cmd_write(path, text);
        }
        else if (eq(cmd, "ps")) cmd_ps();
        else if (eq(cmd, "exit")) { sys_exit(0); for (;;) sys_yield(); }
        else {
            on = 0;
            on = acpy(out, on, "unknown command: ");
            on = acpy(out, on, cmd);
            on = acpy(out, on, "\n");
            sys_write(1, out, on);
        }
    }
}
