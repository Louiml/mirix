/* aurora — MirixOS desktop shell (ring 3, Phase 7a).
 *
 * Creates the desktop windows (About, Uptime, Dock), draws their contents
 * into the shared window buffers, and runs the input loop: title-bar drag,
 * traffic-light close/zoom, Dock hover magnification + icon clicks.
 * All window chrome (menu bar, frames, shadows, titles) is drawn by the
 * kernel compositor; aurora only paints window CONTENT.
 */
#include <stdint.h>

static void memset(void *p, int c, unsigned long n)
{
    unsigned char *d = (unsigned char *)p;
    for (unsigned long i = 0; i < n; i++)
        d[i] = (unsigned char)c;
}

static long syscall3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory");
    return ret;
}
static long syscall6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                      : "rcx", "r11", "memory");
    return ret;
}

#define SYS_write      0
#define SYS_exit       1
#define SYS_gettid     2
#define SYS_yield      3
#define SYS_open       4
#define SYS_read       5
#define SYS_close      6
#define SYS_getdents   7
#define SYS_diskread   9
#define SYS_fbinfo    11
#define SYS_getevent  12
#define SYS_mousepos  13
#define SYS_win_create 14
#define SYS_win_commit 15
#define SYS_win_move   16
#define SYS_win_resize 17
#define SYS_win_close  18
#define SYS_win_focus  19
#define SYS_win_top    20
#define SYS_uptime     21

#define WIN_BUF_VA_BASE   0x40000000UL
#define WIN_BUF_VA_STRIDE 0x400000UL
#define WM_FRAMELESS      1
#define WM_TITLE_H        28

static long sys_write(long fd, const void *b, long n) { return syscall3(SYS_write, fd, (long)b, n); }
static long sys_yield(void) { return syscall3(SYS_yield, 0, 0, 0); }
static long sys_fbinfo(void *b) { return syscall3(SYS_fbinfo, (long)b, 0, 0); }
/* any event (filter=0: kbd first, then mouse) — aurora is the sole keyboard
 * consumer now and forwards keystrokes into the pty master */
static long sys_getevent(void *b) { return syscall3(SYS_getevent, (long)b, 0, 0); }
static long sys_open(const char *p, long fl) { return syscall3(SYS_open, (long)p, fl, 0); }
static long sys_read(long fd, void *b, long n) { return syscall3(SYS_read, fd, (long)b, n); }

#define O_RDONLY 0
#define O_RDWR 2
static long sys_mousepos(void *b) { return syscall3(SYS_mousepos, (long)b, 0, 0); }
static long sys_win_create(int x, int y, int w, int h, const char *t, int fl)
{ return syscall6(SYS_win_create, x, y, w, h, (long)t, fl); }
static long sys_win_commit(int wid) { return syscall3(SYS_win_commit, wid, 0, 0); }
static long sys_win_move(int wid, int x, int y) { return syscall3(SYS_win_move, wid, x, y); }
static long sys_win_resize(int wid, int w, int h) { return syscall3(SYS_win_resize, wid, w, h); }
static long sys_win_close(int wid) { return syscall3(SYS_win_close, wid, 0, 0); }
static long sys_win_focus(int wid) { return syscall3(SYS_win_focus, wid, 0, 0); }
static long sys_win_top(int x, int y) { return syscall3(SYS_win_top, x, y, 0); }
static long sys_uptime(void) { return syscall3(SYS_uptime, 0, 0, 0); }

/* input_event_t must match kernel/drivers/ps2.h */
typedef struct {
    unsigned char type, code, pressed, extended;
    char ascii;
    short dx, dy;
    unsigned char buttons;
} input_event_t;

/* ---------------- tiny string/number helpers ---------------- */
static long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }
static void puts(const char *s) { sys_write(1, s, slen(s)); }
static long acpy(char *d, long n, const char *s) { long l = slen(s); for (long i = 0; i < l; i++) d[n + i] = s[i]; return n + l; }
static long adec(char *d, long n, long v)
{
    char t[24]; long m = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) d[n++] = '-';
    if (u == 0) t[m++] = '0';
    while (u) { t[m++] = (char)('0' + (u % 10)); u /= 10; }
    while (m) d[n++] = t[--m];
    return n;
}
static void log2(const char *a, long v)
{
    char b[96]; long n = 0;
    n = acpy(b, n, a);
    n = adec(b, n, v);
    b[n++] = '\n';
    sys_write(1, b, n);
}

/* ---------------- 8x8 font (public domain, assets/fonts) -------- */
#include "assets/fonts/font8x8_basic.h"

/* Phase 7d: C++ apps (mixtk toolkit) linked into this binary */
#include "apps.h"

/* ---------------- drawing onto a window buffer ---------------- */
static void wpx(uint32_t *buf, int w, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= w) return;
    buf[y * w + x] = c;
}
static void wrect(uint32_t *buf, int w, int h, int x, int y, int rw, int rh, uint32_t c)
{
    for (int yy = y; yy < y + rh && yy < h; yy++)
        for (int xx = x; xx < x + rw && xx < w; xx++)
            if (xx >= 0 && yy >= 0) buf[yy * w + xx] = c;
}
static void wcircle(uint32_t *buf, int w, int h, int cx, int cy, int r, uint32_t c)
{
    for (int yy = -r; yy <= r; yy++)
        for (int xx = -r; xx <= r; xx++)
            if (xx * xx + yy * yy <= r * r) {
                int px = cx + xx, py = cy + yy;
                if (px >= 0 && py >= 0 && px < w && py < h)
                    buf[py * w + px] = c;
            }
}
static void wround(uint32_t *buf, int w, int h, int x, int y, int rw, int rh, int r, uint32_t c)
{
    for (int yy = 0; yy < rh; yy++)
        for (int xx = 0; xx < rw; xx++) {
            int dx = 0, dy = 0, corner = 0;
            if (xx < r && yy < r) { dx = r - xx; dy = r - yy; corner = 1; }
            else if (xx >= rw - r && yy < r) { dx = xx - (rw - r - 1); dy = r - yy; corner = 1; }
            else if (xx < r && yy >= rh - r) { dx = r - xx; dy = yy - (rh - r - 1); corner = 1; }
            else if (xx >= rw - r && yy >= rh - r) { dx = xx - (rw - r - 1); dy = yy - (rh - r - 1); corner = 1; }
            if (corner && dx * dx + dy * dy > r * r) continue;
            int px = x + xx, py = y + yy;
            if (px >= 0 && py >= 0 && px < w && py < h) buf[py * w + px] = c;
        }
}
static void wtext(uint32_t *buf, int w, int h, int x, int y, int scale, uint32_t c, const char *s)
{
    int cx = x;
    for (int i = 0; s[i]; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch >= 128) ch = '?';
        for (int row = 0; row < 8; row++) {
            unsigned char bits = font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    wrect(buf, w, h, cx + col * scale, y + row * scale, scale, scale, c);
        }
        cx += 8 * scale;
    }
}

/* ---------------- window bookkeeping ---------------- */
#define MAXW 8
typedef struct {
    int used, wid, x, y, w, h, frameless;
    const char *title;
    uint32_t *buf;
} awin_t;
static awin_t wins[MAXW];
static int nwins;
static int SW, SH;   /* screen size */

static awin_t *find_wid(int wid)
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].wid == wid)
            return &wins[i];
    return 0;
}

/* Phase 7d: live options driven by the Settings app (user/settings.cpp) */
static int g_clock_seconds = 1;
static int g_term_dark = 1;
static int g_dock_mag = 1;
void aurora_set_clock_seconds(int on) { g_clock_seconds = on; }
void aurora_set_term_dark(int on) { g_term_dark = on ? 1 : 0; }
void aurora_set_dock_magnify(int on) { g_dock_mag = on ? 1 : 0; }

/* Phase 7d app windows (owned by this shell, painted by the C++ apps) */
static awin_t *editor_win;
static awin_t *settings_win;
static int kbd_editor;      /* 1 = keystrokes go to the Editor, else to the pty */

static awin_t *add_win(int x, int y, int w, int h, const char *title, int fl)
{
    for (int i = 0; i < MAXW; i++) {
        if (wins[i].used) continue;
        long wid = sys_win_create(x, y, w, h, title, fl);
        if (wid < 0) return 0;
        wins[i].used = 1;
        wins[i].wid = (int)wid;
        wins[i].x = x; wins[i].y = y; wins[i].w = w; wins[i].h = h;
        wins[i].frameless = fl;
        wins[i].title = title;
        wins[i].buf = (uint32_t *)(WIN_BUF_VA_BASE + (unsigned long)wid * WIN_BUF_VA_STRIDE);
        nwins++;
        return &wins[i];
    }
    return 0;
}

static void del_win(int wid)
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].wid == wid) {
            wins[i].used = 0;
            nwins--;
            /* Phase 7d: detach the C++ app driving this window */
            if (editor_win && editor_win->wid == wid) { editor_detach(); editor_win = 0; kbd_editor = 0; }
            if (settings_win && settings_win->wid == wid) { settings_detach(); settings_win = 0; }
            sys_win_close(wid);
            return;
        }
}

/* ---------------- window contents ---------------- */

static void draw_about(awin_t *a)
{
    uint32_t *b = a->buf;
    int w = a->w, h = a->h;
    wrect(b, w, h, 0, 0, w, h, 0xf7f7fa);
    /* aurora gradient banner */
    for (int y = 0; y < 70; y++) {
        uint32_t t = (uint32_t)(y * 255 / 70);
        uint32_t r = 60 + t / 3, g = 40 + (t * 2) / 3, bl = 160 + (t / 4);
        wrect(b, w, h, 0, y, w, 1, (r << 16) | (g << 8) | bl);
    }
    wtext(b, w, h, 16, 16, 4, 0xffffff, "MirixOS");
    wtext(b, w, h, 16, 52, 2, 0xfff0c0, "Aurora desktop");
    wtext(b, w, h, 16, 96, 1, 0x333344, "64-bit hobby OS — Limine boot, ring-3 userland");
    wtext(b, w, h, 16, 110, 1, 0x333344, "kernel compositor + TrueType + AHCI + PS/2");
    wtext(b, w, h, 16, 132, 1, 0x666677, "drag the title bar, click red to close,");
    wtext(b, w, h, 16, 144, 1, 0x666677, "green to zoom, hover the Dock below");
    wcircle(b, w, h, w - 40, h - 40, 18, 0x30ffa0);
    wcircle(b, w, h, w - 40, h - 40, 10, 0xb060ff);
}

static void draw_uptime(awin_t *a)
{
    uint32_t *b = a->buf;
    int w = a->w, h = a->h;
    wrect(b, w, h, 0, 0, w, h, 0x1c1c28);
    long ms = sys_uptime();
    char line[64];
    long n = 0;
    n = acpy(line, n, "uptime: ");
    n = adec(line, n, ms / 1000);
    if (g_clock_seconds) {
        n = acpy(line, n, ".");
        long frac = (ms / 100) % 10;
        line[n++] = (char)('0' + frac);
    }
    line[n++] = 's';
    line[n] = 0;
    wtext(b, w, h, 14, 16, 2, 0x66ffcc, "Clock");
    wtext(b, w, h, 14, 48, 2, 0xffffff, line);
    if (g_clock_seconds)
        wtext(b, w, h, 14, 80, 1, 0x8899aa, "ticks at 100 Hz");
    else
        wtext(b, w, h, 14, 80, 1, 0x8899aa, "seconds hidden (Settings)");
    /* little progress bar */
    wrect(b, w, h, 14, h - 30, w - 28, 8, 0x333344);
    wrect(b, w, h, 14, h - 30, (w - 28) * (int)(ms % 5000) / 5000, 8, 0x40d0ff);
}

#define DOCK_ICONS 6
static const char *dock_names[DOCK_ICONS] = { "About", "Clock", "Files", "Terminal", "Editor", "Settings" };
static const uint32_t dock_cols[DOCK_ICONS] = { 0x30ffa0, 0x40d0ff, 0xb060ff, 0xff9f40, 0x70e0ff, 0xf0f060 };

static void draw_dock(awin_t *d, int mx, int hover_icon)
{
    uint32_t *b = d->buf;
    int w = d->w, h = d->h;
    /* translucent pill */
    wround(b, w, h, 0, 0, w, h, 20, 0x20202c);
    int base = 44, gap = 14;
    int total = DOCK_ICONS * (base + gap) - gap;
    int x0 = (w - total) / 2;
    int ybase = h - 12;
    for (int i = 0; i < DOCK_ICONS; i++) {
        int icx = x0 + i * (base + gap) + base / 2;
        int size = base;
        int scale_pct = 100;
        if (g_dock_mag && mx >= 0 && hover_icon == i) {
            /* magnification falls off with distance to the icon center */
            int dx = mx - icx;
            if (dx < 0) dx = -dx;
            if (dx < 70) {
                scale_pct = 100 + (70 - dx) * 60 / 70;   /* up to 160% */
                size = base * scale_pct / 100;
            }
        }
        int ix = icx - size / 2;
        int iy = ybase - size;
        wround(b, w, h, ix, iy, size, size, size / 5, dock_cols[i]);
        /* letter */
        char s[2] = { dock_names[i][0], 0 };
        wtext(b, w, h, ix + size / 2 - 4, iy + size / 2 - 4, 1, 0xffffff, s);
        (void)scale_pct;
    }
}

/* ---------------- Files (Navigator) window — Phase 7c ---------------- */
typedef struct { char name[32]; uint32_t size; uint8_t type; uint8_t pad[3]; } dirent_t;

#define FILES_HDR  24
#define FILES_ROWH 12
static dirent_t fents[32];
static int  nfents;
static char fcontent[512];
static int  fclen;

static int eqs(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static awin_t *find_title(const char *t)
{
    for (int i = 0; i < MAXW; i++)
        if (wins[i].used && wins[i].title && eqs(wins[i].title, t))
            return &wins[i];
    return 0;
}

static void files_refresh(void)
{
    nfents = 0;
    fclen = 0;
    long fd = sys_open("/", O_RDONLY);
    if (fd < 0)
        return;
    char dbuf[1024];
    long nb = syscall3(SYS_getdents, fd, (long)dbuf, (long)sizeof dbuf);
    syscall3(SYS_close, fd, 0, 0);
    if (nb <= 0)
        return;
    long cnt = nb / (long)sizeof(dirent_t);
    if (cnt > 32)
        cnt = 32;
    for (long i = 0; i < cnt; i++)
        fents[i] = *(dirent_t *)(dbuf + i * (long)sizeof(dirent_t));
    nfents = (int)cnt;
    char m[80]; long n = 0;
    n = acpy(m, n, "aurora: files: ");
    n = adec(m, n, nfents);
    n = acpy(m, n, " entries in /\n");
    sys_write(1, m, n);
}

static void files_open_row(int row)
{
    if (row < 0)
        return;
    if (row < nfents) {
        long fd = sys_open(fents[row].name, O_RDONLY);
        if (fd < 0) {
            puts("aurora: files: open failed\n");
            return;
        }
        fclen = sys_read(fd, fcontent, (long)sizeof fcontent - 1);
        syscall3(SYS_close, fd, 0, 0);
        if (fclen < 0)
            fclen = 0;
        fcontent[fclen] = 0;
        char m[160]; long n = 0;
        n = acpy(m, n, "aurora: files: read '");
        n = acpy(m, n, fents[row].name);
        n = acpy(m, n, "' (");
        n = adec(m, n, fclen);
        n = acpy(m, n, " bytes): ");
        for (long i = 0; i < fclen && n < 140; i++) {
            char c = fcontent[i];
            m[n++] = (c == '\n' || c < 32 || c >= 127) ? '.' : c;
        }
        n = acpy(m, n, "\n");
        sys_write(1, m, n);
    } else {
        /* the row after the last file = the AHCI SATA disk */
        static char dbuf[8 * 512];
        long nrd = syscall3(SYS_diskread, 0, 8, (long)dbuf);
        char m[96]; long n = 0;
        n = acpy(m, n, "aurora: files: ahci LBA 0 read ");
        n = adec(m, n, nrd);
        n = acpy(m, n, " bytes, magic '");
        for (int i = 0; i < 10 && i < nrd; i++) {
            char c = dbuf[i];
            m[n++] = (c >= 32 && c < 127) ? c : '.';
        }
        n = acpy(m, n, "'\n");
        sys_write(1, m, n);
    }
}

static void draw_files(awin_t *a)
{
    uint32_t *b = a->buf;
    int w = a->w, h = a->h;
    wrect(b, w, h, 0, 0, w, h, 0xf4f4f8);
    char hdr[64]; long hn = 0;
    hn = acpy(hdr, hn, "Files - / (");
    hn = adec(hdr, hn, nfents);
    hn = acpy(hdr, hn, " entries)");
    hdr[hn] = 0;
    wtext(b, w, h, 8, 6, 1, 0x3a3a44, hdr);
    wrect(b, w, h, 4, FILES_HDR - 4, w - 8, 1, 0xc0c0cc);
    for (int i = 0; i <= nfents; i++) {
        int y = FILES_HDR + i * FILES_ROWH;
        if (y + FILES_ROWH > h)
            break;
        if (i == nfents) {
            wrect(b, w, h, 4, y, w - 8, FILES_ROWH - 2, 0xffe8c8);
            wtext(b, w, h, 10, y + 2, 1, 0x8a5a10, "[ahci0 SATA disk - LBA 0]");
        } else {
            wrect(b, w, h, 4, y, w - 8, FILES_ROWH - 2, 0xe8e8f0);
            char row[64]; long rn = 0;
            rn = acpy(row, rn, fents[i].name);
            rn = acpy(row, rn, "  (");
            rn = adec(row, rn, (long)fents[i].size);
            rn = acpy(row, rn, " B)");
            row[rn] = 0;
            wtext(b, w, h, 10, y + 2, 1, 0x2a2a33, row);
        }
    }
    /* content preview of the last opened file */
    int cy = FILES_HDR + (nfents + 1) * FILES_ROWH + 6;
    if (cy < h) {
        wrect(b, w, h, 4, cy - 4, w - 8, 1, 0xc0c0cc);
        int cx = 8, cyy = cy + 2;
        for (int i = 0; i < fclen && cyy + 8 <= h; i++) {
            char c = fcontent[i];
            if (c == '\n' || cx + 8 > w - 8) {
                cx = 8;
                cyy += 8;
                if (cyy + 8 > h)
                    break;
                if (c == '\n')
                    continue;
            }
            char s[2] = { (c >= 32 && c < 127) ? c : '.', 0 };
            wtext(b, w, h, cx, cyy, 1, 0x1a4a2a, s);
            cx += 8;
        }
    }
}

/* ---------------- terminal window (Phase 7b) ---------------- */
#define TCOLS 55
#define TROWS 34
static char tscreen[TROWS][TCOLS + 1];
static int tcol, trow;
static int term_dirty;

static void term_scroll(void)
{
    for (int y = 0; y + 1 < TROWS; y++) {
        for (int x = 0; x <= TCOLS; x++)
            tscreen[y][x] = tscreen[y + 1][x];
    }
    for (int x = 0; x <= TCOLS; x++)
        tscreen[TROWS - 1][x] = ' ';
    tscreen[TROWS - 1][TCOLS] = 0;
}

static void term_putc(char c)
{
    if (c == '\n') {
        tcol = 0;
        if (++trow >= TROWS) { term_scroll(); trow = TROWS - 1; }
        return;
    }
    if (c == '\r')
        return;
    if (c == '\b') {
        if (tcol > 0) tcol--;
        else if (trow > 0) { trow--; tcol = TCOLS - 1; }
        tscreen[trow][tcol] = ' ';
        return;
    }
    if (c < 32)
        return;
    tscreen[trow][tcol] = c;
    if (++tcol >= TCOLS) {
        tcol = 0;
        if (++trow >= TROWS) { term_scroll(); trow = TROWS - 1; }
    }
    term_dirty = 1;
}

static void draw_term(awin_t *a)
{
    uint32_t *b = a->buf;
    int w = a->w, h = a->h;
    uint32_t bg = g_term_dark ? 0x101018 : 0xf4f4f0;
    uint32_t fg = g_term_dark ? 0xc8ffc8 : 0x1a5a2a;
    wrect(b, w, h, 0, 0, w, h, bg);
    for (int y = 0; y < TROWS; y++) {
        int yy = y * 8 + 4;
        if (yy + 8 > h) break;
        char line[TCOLS + 1];
        for (int x = 0; x < TCOLS; x++)
            line[x] = tscreen[y][x] ? tscreen[y][x] : ' ';
        line[TCOLS] = 0;
        wtext(b, w, h, 4, yy, 1, fg, line);
    }
    /* cursor block */
    wrect(b, w, h, 4 + tcol * 8, trow * 8 + 4, 8, 8, 0x40d0ff);
}

/* ---------------- main loop ---------------- */

long main(void)
{
    uint32_t fb[3];
    sys_fbinfo(fb);
    SW = (int)fb[0];
    SH = (int)fb[1];

    awin_t *about = add_win(120, 70, 380, 260, "About MirixOS", 0);
    awin_t *clock = add_win(560, 90, 240, 150, "Uptime", 0);
    awin_t *term  = add_win(430, 160, 440, 300, "Terminal", 0);
    int dw = DOCK_ICONS * (44 + 14) - 14 + 24;
    int dh = 44 + 26;
    awin_t *dock = add_win((SW - dw) / 2, SH - dh - 12, dw, dh, "Dock", WM_FRAMELESS);
    if (!about || !clock || !term || !dock) {
        puts("aurora: FAILED to create windows\n");
        syscall3(SYS_exit, 1, 0, 0);
        return 1;
    }
    draw_about(about);
    draw_uptime(clock);
    draw_dock(dock, -1, -1);
    memset(tscreen, ' ', sizeof tscreen);
    for (int y = 0; y < TROWS; y++)
        tscreen[y][TCOLS] = 0;
    tcol = 0; trow = 0;
    draw_term(term);
    sys_win_commit(about->wid);
    sys_win_commit(clock->wid);
    sys_win_commit(term->wid);
    sys_win_commit(dock->wid);
    sys_win_focus(about->wid);

    /* Phase 7b: open the pty master — mirsh runs on the slave side. */
    long mfd = sys_open("/ptym0", O_RDWR);
    if (mfd < 0) {
        puts("aurora: FAILED to open /ptym0\n");
    } else {
        char m[64]; long n = 0;
        n = acpy(m, n, "aurora: pty link up (fd ");
        n = adec(m, n, mfd);
        n = acpy(m, n, ") — mirsh runs in the Terminal window\n");
        sys_write(1, m, n);
    }

    {
        char m[96]; long n = 0;
        n = acpy(m, n, "aurora: ready: ");
        n = adec(m, n, nwins);
        n = acpy(m, n, " windows (About, Uptime, Terminal, Dock) on ");
        n = adec(m, n, SW);
        m[n++] = 'x';
        n = adec(m, n, SH);
        n = acpy(m, n, "\n");
        sys_write(1, m, n);
    }

    int mx = -1, my = -1, btns = 0, prev_btns = 0;
    int last_mx = -2, last_my = -2, last_btns = -1;   /* for [mouse] echo */
    int drag_wid = -1, drag_dx = 0, drag_dy = 0;
    int hover_icon = -1;
    int last_uptime_redraw = -1;

    for (;;) {
        input_event_t ev;
        while (sys_getevent(&ev)) {
            if (ev.type == 1) {
                mx += ev.dx;
                my += ev.dy;
                btns = ev.buttons;
            } else if (ev.type == 0 && ev.pressed && ev.ascii) {
                char c = ev.ascii;
                /* Phase 7d: the Editor window owns the keyboard while focused */
                if (kbd_editor && editor_win && find_wid(editor_win->wid)) {
                    if (editor_key(c))
                        editor_paint();
                } else if (mfd >= 0) {
                    /* forward the keystroke to the shell on the pty slave */
                    sys_write(mfd, &c, 1);
                }
            }
        }
        /* drain shell output from the pty master: mirror to serial (so the
         * headless test sees it) and render into the Terminal window */
        if (mfd >= 0) {
            char pb[256];
            long pn;
            while ((pn = sys_read(mfd, pb, (long)sizeof pb)) > 0) {
                sys_write(1, pb, pn);
                for (long i = 0; i < pn; i++)
                    term_putc(pb[i]);
            }
            if (term_dirty && term && find_wid(term->wid)) {
                term_dirty = 0;
                draw_term(term);
                sys_win_commit(term->wid);
            }
        }
        {
            uint32_t mp[3];
            sys_mousepos(mp);
            if (mx < 0) { mx = (int)mp[0]; my = (int)mp[1]; }
            else { mx = (int)mp[0]; my = (int)mp[1]; }
            btns = (int)mp[2];
        }

        /* [mouse] echo on change (same format as the kernel's main loop, so
         * the headless test sees it even if the main loop loses the race for
         * the event ring — position here comes from SYS_mousepos, not the
         * ring, so aurora never starves) */
        if (mx != last_mx || my != last_my || btns != last_btns) {
            char m[48]; long n = 0;
            n = acpy(m, n, "[mouse] x=");
            n = adec(m, n, mx);
            n = acpy(m, n, " y=");
            n = adec(m, n, my);
            n = acpy(m, n, " b=");
            int hi = (btns >> 4) & 0xf, lo = btns & 0xf;
            m[n++] = (char)(hi < 10 ? '0' + hi : 'a' + hi - 10);
            m[n++] = (char)(lo < 10 ? '0' + lo : 'a' + lo - 10);
            m[n++] = '\n';
            sys_write(1, m, n);
            last_mx = mx; last_my = my; last_btns = btns;
        }

        /* button down: hit-test traffic lights / title bar / dock */
        if ((btns & 1) && !(prev_btns & 1)) {
            int wid = (int)sys_win_top(mx, my);
            awin_t *hit = wid >= 0 ? find_wid(wid) : 0;
            if (hit && !hit->frameless) {
                int lx = mx - hit->x, ly = my - hit->y;
                if (ly < WM_TITLE_H) {
                    /* traffic lights? red=close green=zoom */
                    int d_red = (lx - 14) * (lx - 14) + (ly - WM_TITLE_H / 2) * (ly - WM_TITLE_H / 2);
                    int d_grn = (lx - 54) * (lx - 54) + (ly - WM_TITLE_H / 2) * (ly - WM_TITLE_H / 2);
                    if (d_red <= 49) {
                        char m[64]; long n = 0;
                        n = acpy(m, n, "aurora: closed win ");
                        n = adec(m, n, wid);
                        n = acpy(m, n, " (");
                        n = acpy(m, n, hit->title);
                        n = acpy(m, n, ")\n");
                        sys_write(1, m, n);
                        sys_win_focus(wid);
                        del_win(wid);
                    } else if (d_grn <= 49) {
                        sys_win_resize(wid, 640, 480);
                        hit->w = 640; hit->h = 480;
                        char m[64]; long n = 0;
                        n = acpy(m, n, "aurora: zoom win ");
                        n = adec(m, n, wid);
                        n = acpy(m, n, " to 640x480\n");
                        sys_write(1, m, n);
                        if (eqs(hit->title, "Uptime")) draw_uptime(hit);
                        else if (eqs(hit->title, "About MirixOS")) draw_about(hit);
                        else if (eqs(hit->title, "Files")) { files_refresh(); draw_files(hit); }
                        else if (eqs(hit->title, "Terminal")) draw_term(hit);
                        else if (hit == editor_win) editor_attach(wid, hit->buf, hit->x, hit->y, 640, 480);
                        else if (hit == settings_win) settings_attach(wid, hit->buf, hit->x, hit->y, 640, 480);
                        sys_win_commit(wid);
                    } else {
                        drag_wid = wid;
                        drag_dx = lx;
                        drag_dy = ly;
                        sys_win_focus(wid);
                    }
                } else {
                    sys_win_focus(wid);
                    /* Phase 7d: the Editor window takes the keyboard when clicked */
                    if (hit == editor_win) {
                        kbd_editor = 1;
                        editor_click(lx, ly);
                    } else {
                        if (hit == term) kbd_editor = 0;
                        /* Files window: click a row to open the file / disk */
                        awin_t *fw = find_title("Files");
                        if (hit == fw) {
                            int row = (ly - WM_TITLE_H - FILES_HDR) / FILES_ROWH;
                            if (row >= 0 && row <= nfents) {
                                files_open_row(row);
                                draw_files(hit);
                                sys_win_commit(hit->wid);
                            }
                        } else if (hit == settings_win) {
                            settings_click(lx, ly);
                        }
                    }
                }
            } else if (hit && hit->frameless) {
                /* dock click: which icon? */
                int base = 44, gap = 14;
                int total = DOCK_ICONS * (base + gap) - gap;
                int x0 = hit->x + (hit->w - total) / 2;
                for (int i = 0; i < DOCK_ICONS; i++) {
                    int icx = x0 + i * (base + gap) + base / 2;
                    if (mx >= icx - base && mx <= icx + base && my >= hit->y) {
                        char m[80]; long n = 0;
                        n = acpy(m, n, "aurora: dock launch '");
                        n = acpy(m, n, dock_names[i]);
                        n = acpy(m, n, "'\n");
                        sys_write(1, m, n);
                        if (i == 0) {
                            if (find_wid(about->wid)) sys_win_focus(about->wid);
                        } else if (i == 1) {
                            if (find_wid(clock->wid)) sys_win_focus(clock->wid);
                        } else if (i == 2) {
                            /* Files (Navigator) */
                            awin_t *fw = find_title("Files");
                            if (!fw) {
                                fw = add_win(640, 200, 360, 300, "Files", 0);
                                if (fw) {
                                    files_refresh();
                                    draw_files(fw);
                                    sys_win_commit(fw->wid);
                                    sys_win_focus(fw->wid);
                                    puts("aurora: dock launch 'Files'\n");
                                }
                            } else {
                                sys_win_focus(fw->wid);
                                puts("aurora: dock launch 'Files' (already open, focused)\n");
                            }
                        } else if (i == 3) {
                            if (term && find_wid(term->wid)) {
                                sys_win_focus(term->wid);
                                kbd_editor = 0;
                                puts("aurora: dock launch 'Terminal'\n");
                            }
                        } else if (i == 4) {
                            /* Phase 7d: Text Editor (C++ mixtk app) */
                            if (editor_win && find_wid(editor_win->wid)) {
                                sys_win_focus(editor_win->wid);
                                kbd_editor = 1;
                                puts("aurora: dock launch 'Editor' (already open, focused)\n");
                            } else {
                                editor_win = add_win(200, 120, 520, 340, "Editor", 0);
                                if (editor_win) {
                                    kbd_editor = 1;
                                    editor_attach(editor_win->wid, editor_win->buf,
                                                  editor_win->x, editor_win->y,
                                                  editor_win->w, editor_win->h);
                                    sys_win_focus(editor_win->wid);
                                }
                            }
                        } else if (i == 5) {
                            /* Phase 7d: System Settings (C++ mixtk app) */
                            if (settings_win && find_wid(settings_win->wid)) {
                                sys_win_focus(settings_win->wid);
                                puts("aurora: dock launch 'Settings' (already open, focused)\n");
                            } else {
                                settings_win = add_win(740, 140, 320, 240, "Settings", 0);
                                if (settings_win) {
                                    settings_attach(settings_win->wid, settings_win->buf,
                                                    settings_win->x, settings_win->y,
                                                    settings_win->w, settings_win->h);
                                    sys_win_focus(settings_win->wid);
                                }
                            }
                        }
                        break;
                    }
                }
            }
        }
        /* drag */
        if (drag_wid >= 0 && (btns & 1)) {
            awin_t *d = find_wid(drag_wid);
            if (d) {
                int nx = mx - drag_dx, ny = my - drag_dy;
                if (nx != d->x || ny != d->y) {
                    sys_win_move(drag_wid, nx, ny);
                    d->x = nx; d->y = ny;
                }
            }
        }
        if (!(btns & 1) && (prev_btns & 1) && drag_wid >= 0) {
            awin_t *d = find_wid(drag_wid);
            char m[80]; long n = 0;
            n = acpy(m, n, "aurora: drag win ");
            n = adec(m, n, drag_wid);
            n = acpy(m, n, " -> (");
            n = adec(m, n, d ? d->x : 0);
            m[n++] = ',';
            n = adec(m, n, d ? d->y : 0);
            n = acpy(m, n, ")\n");
            sys_write(1, m, n);
            drag_wid = -1;
        }
        prev_btns = btns;

        /* dock hover magnification */
        if (dock && find_wid(dock->wid)) {
            int base = 44, gap = 14;
            int total = DOCK_ICONS * (base + gap) - gap;
            int x0 = dock->x + (dock->w - total) / 2;
            int new_hover = -1;
            if (my >= dock->y && my <= dock->y + dock->h) {
                for (int i = 0; i < DOCK_ICONS; i++) {
                    int icx = x0 + i * (base + gap) + base / 2;
                    if (mx >= icx - 70 && mx <= icx + 70) { new_hover = i; break; }
                }
            }
            if (new_hover != hover_icon) {
                hover_icon = new_hover;
                draw_dock(dock, mx, hover_icon);
                sys_win_commit(dock->wid);
                if (hover_icon >= 0) {
                    char m[64]; long n = 0;
                    n = acpy(m, n, "aurora: dock hover icon ");
                    n = adec(m, n, hover_icon);
                    n = acpy(m, n, " '");
                    n = acpy(m, n, dock_names[hover_icon]);
                    n = acpy(m, n, "'\n");
                    sys_write(1, m, n);
                }
            }
        }

        /* uptime redraw ~2 Hz */
        long up = sys_uptime() / 500;
        if (clock && find_wid(clock->wid) && up != last_uptime_redraw) {
            last_uptime_redraw = up;
            draw_uptime(clock);
            sys_win_commit(clock->wid);
        }

        /* Phase 7d: repaint C++ app windows when their state changed */
        if (editor_win && find_wid(editor_win->wid) && editor_dirty())
            editor_paint();
        if (settings_win && find_wid(settings_win->wid) && settings_dirty())
            settings_paint();

        sys_yield();
    }
    return 0;
}
