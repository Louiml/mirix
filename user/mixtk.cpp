/* mixtk.cpp — MirixOS widget toolkit implementation (Phase 7d).
 * Freestanding C++17: provides the compiler-emitted mem* shims (clang may
 * call them for struct copies) plus syscall wrappers, 8x8-font drawing and
 * the Widget/App classes. */
#include "mixtk.h"

/* font8x8_basic is defined (non-static) in assets/fonts/font8x8_basic.h,
 * which aurora.c already includes — just reference it here. */
extern char font8x8_basic[128][8];

/* ---- compiler builtins (clang emits calls to these even freestanding) ---- */
extern "C" {
void *memset(void *d, int c, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    for (unsigned long i = 0; i < n; i++) p[i] = (unsigned char)c;
    return d;
}
void *memcpy(void *d, const void *s, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    for (unsigned long i = 0; i < n; i++) p[i] = q[i];
    return d;
}
void *memmove(void *d, const void *s, unsigned long n)
{
    unsigned char *p = (unsigned char *)d;
    const unsigned char *q = (const unsigned char *)s;
    if (p < q) { for (unsigned long i = 0; i < n; i++) p[i] = q[i]; }
    else { for (unsigned long i = n; i > 0; i--) p[i - 1] = q[i - 1]; }
    return d;
}
int memcmp(const void *a, const void *b, unsigned long n)
{
    const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
    for (unsigned long i = 0; i < n; i++)
        if (p[i] != q[i]) return p[i] < q[i] ? -1 : 1;
    return 0;
}
unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; }
void __cxa_pure_virtual() { for (;;) {} }
/* freestanding: no runtime — global dtors are registered via atexit (noop)
 * and virtual destructors reference sized operator delete */
int atexit(void (*)(void)) { return 0; }
}
void operator delete(void *) noexcept {}
void operator delete(void *, unsigned long) noexcept {}
void *operator new(unsigned long n, void *p) noexcept { (void)n; return p; }

/* ---- syscalls ---- */
namespace sys {

long call3(long nr, long a1, long a2, long a3)
{
    long ret;
    __asm__ volatile ("syscall"
                      : "=a"(ret)
                      : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                      : "rcx", "r11", "memory");
    return ret;
}
long call6(long nr, long a1, long a2, long a3, long a4, long a5, long a6)
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
long write(long fd, const void *b, long n) { return call3(SYS_write, fd, (long)b, n); }
long open(const char *path, long flags)    { return call3(SYS_open, (long)path, flags, 0); }
long read(long fd, void *b, long n)        { return call3(SYS_read, fd, (long)b, n); }
long close(long fd)                        { return call3(SYS_close, fd, 0, 0); }
long win_create(int x, int y, int w, int h, const char *t, int fl)
{ return call6(SYS_win_create, x, y, w, h, (long)t, fl); }
long win_commit(int wid) { return call3(SYS_win_commit, wid, 0, 0); }
long win_close(int wid)  { return call3(SYS_win_close, wid, 0, 0); }
long win_focus(int wid)  { return call3(SYS_win_focus, wid, 0, 0); }
long win_top(int x, int y){ return call3(SYS_win_top, x, y, 0); }
long uptime(void)        { return call3(SYS_uptime, 0, 0, 0); }

void log(const char *s) { write(1, s, gfx::slen(s)); }

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
static long acpy(char *d, long n, const char *s)
{
    long l = gfx::slen(s);
    for (long i = 0; i < l; i++) d[n + i] = s[i];
    return n + l;
}
void log2(const char *a, long v)
{
    char b[96]; long n = 0;
    n = acpy(b, n, a);
    n = adec(b, n, v);
    b[n++] = '\n';
    write(1, b, n);
}
void log3(const char *a, const char *b2, long v)
{
    char b[128]; long n = 0;
    n = acpy(b, n, a);
    n = acpy(b, n, b2);
    n = adec(b, n, v);
    b[n++] = '\n';
    write(1, b, n);
}

} // namespace sys

/* ---- drawing ---- */
namespace gfx {

long slen(const char *s) { long n = 0; while (s[n]) n++; return n; }

void px(Surf &s, int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= s.w || y >= s.h) return;
    s.px[y * s.w + x] = c;
}
void rect(Surf &s, int x, int y, int w, int h, uint32_t c)
{
    for (int yy = y; yy < y + h && yy < s.h; yy++)
        for (int xx = x; xx < x + w && xx < s.w; xx++)
            if (xx >= 0 && yy >= 0) s.px[yy * s.w + xx] = c;
}
void rrect(Surf &s, int x, int y, int w, int h, int r, uint32_t c)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            int dx = 0, dy = 0, corner = 0;
            if (xx < r && yy < r) { dx = r - xx; dy = r - yy; corner = 1; }
            else if (xx >= w - r && yy < r) { dx = xx - (w - r - 1); dy = r - yy; corner = 1; }
            else if (xx < r && yy >= h - r) { dx = r - xx; dy = yy - (h - r - 1); corner = 1; }
            else if (xx >= w - r && yy >= h - r) { dx = xx - (w - r - 1); dy = yy - (h - r - 1); corner = 1; }
            if (corner && dx * dx + dy * dy > r * r) continue;
            px(s, x + xx, y + yy, c);
        }
}
void circle(Surf &s, int cx, int cy, int r, uint32_t c)
{
    for (int yy = -r; yy <= r; yy++)
        for (int xx = -r; xx <= r; xx++)
            if (xx * xx + yy * yy <= r * r) px(s, cx + xx, cy + yy, c);
}
void text(Surf &s, int x, int y, int scale, uint32_t c, const char *str)
{
    int cx = x;
    for (int i = 0; str[i]; i++) {
        unsigned char ch = (unsigned char)str[i];
        if (ch >= 128) ch = '?';
        for (int row = 0; row < 8; row++) {
            unsigned char bits = (unsigned char)font8x8_basic[ch][row];
            for (int col = 0; col < 8; col++)
                if (bits & (0x80 >> col))
                    rect(s, cx + col * scale, y + row * scale, scale, scale, c);
        }
        cx += 8 * scale;
    }
}
int textw(int scale, const char *str) { return (int)slen(str) * 8 * scale; }

} // namespace gfx

/* ---- widgets ---- */
namespace mixtk {

void Label::paint(gfx::Surf &s, bool hover, bool pressed)
{
    (void)hover; (void)pressed;
    gfx::text(s, x, y, scale, color, text);
}

void Button::paint(gfx::Surf &s, bool hover, bool pressed)
{
    uint32_t f = face;
    if (pressed) f = edge;
    else if (hover) f = 0x6a9bff;   /* lighter */
    gfx::rrect(s, x, y, w, h, 6, f);
    gfx::rrect(s, x, y, w, h, 6, edge);
    int tw = gfx::textw(1, label);
    gfx::text(s, x + (w - tw) / 2, y + (h - 8) / 2, 1, ink, label);
}

bool Button::on_click(int lx, int ly)
{
    if (!hit(lx, ly)) return false;
    if (on_activate) on_activate(ctx);
    return true;
}

void Toggle::paint(gfx::Surf &s, bool hover, bool pressed)
{
    (void)pressed;
    uint32_t row = hover ? 0xe8ecf8 : 0xf4f5fa;
    gfx::rrect(s, x, y, w, h, 5, row);
    /* checkbox */
    gfx::rect(s, x + 8, y + (h - 14) / 2, 14, 14, value ? 0x3a7dff : 0xffffff);
    if (value)
        gfx::text(s, x + 10, y + (h - 14) / 2 + 3, 1, 0xffffff, "x");
    gfx::text(s, x + 30, y + (h - 8) / 2, 1, ink, name);
}

bool Toggle::on_click(int lx, int ly)
{
    if (!hit(lx, ly)) return false;
    value = !value;
    if (on_change) on_change(ctx, value);
    return true;
}

void App::attach(int wid, uint32_t *buf, int x, int y, int w, int h)
{
    win.wid = wid; win.buf = buf; win.x = x; win.y = y; win.w = w; win.h = h;
    dirty = true;
}

gfx::Surf App::surf()
{
    gfx::Surf s;
    s.px = win.buf;
    s.w = win.w;
    s.h = win.h > WM_TITLE_H ? win.h - WM_TITLE_H : win.h;
    return s;
}

void App::paint()
{
    gfx::Surf s = surf();
    /* widgets are addressed in content coords (title bar stripped): paint
     * them onto a surf whose origin is shifted down by WM_TITLE_H. */
    gfx::Surf cs;
    cs.px = win.buf + WM_TITLE_H * win.w;
    cs.w = win.w;
    cs.h = win.h - WM_TITLE_H;
    paint_content(cs);
    for (int i = 0; i < nwidgets; i++)
        widgets[i]->paint(cs, false, false);
    sys::win_commit(win.wid);
    dirty = false;
    (void)s;
}

bool App::click(int lx, int ly)
{
    /* content coords: ly is relative to the top of the window; strip title */
    int cy = ly - WM_TITLE_H;
    for (int i = nwidgets - 1; i >= 0; i--)
        if (widgets[i]->on_click(lx, cy)) { dirty = true; return true; }
    return false;
}

} // namespace mixtk
