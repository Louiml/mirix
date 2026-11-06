/* mixtk.h — MirixOS widget toolkit (Phase 7d), freestanding C++17.
 *
 * No libc/libstdc++: syscall wrappers + software drawing onto a kernel window
 * buffer + a small widget set (Label/Button/Toggle) owned by an App.
 * Apps are linked into the aurora binary and driven by its input loop. */
#ifndef MIRIX_USER_MIXTK_H
#define MIRIX_USER_MIXTK_H

#include <stdint.h>

/* ---- syscall numbers (mirror kernel/proc/syscall.h) ---- */
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

/* ---- open() flags (mirror kernel/fs/vfs.h) ---- */
#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR   2
#define O_CREAT  0x40
#define O_TRUNC  0x80

#define WIN_BUF_VA_BASE   0x40000000UL
#define WIN_BUF_VA_STRIDE 0x400000UL
#define WM_FRAMELESS      1
#define WM_TITLE_H        28

namespace sys {
    long call3(long nr, long a1, long a2, long a3);
    long call6(long nr, long a1, long a2, long a3, long a4, long a5, long a6);
    long write(long fd, const void *b, long n);
    long open(const char *path, long flags);
    long read(long fd, void *b, long n);
    long close(long fd);
    long win_create(int x, int y, int w, int h, const char *title, int flags);
    long win_commit(int wid);
    long win_close(int wid);
    long win_focus(int wid);
    long win_top(int x, int y);
    long uptime(void);
    void log(const char *s);
    void log2(const char *a, long v);          /* a + dec(v) + '\n' */
    void log3(const char *a, const char *b, long v); /* a + b + dec(v) + '\n' */
}

namespace gfx {
    struct Surf { uint32_t *px; int w, h; };
    void px(Surf &s, int x, int y, uint32_t c);
    void rect(Surf &s, int x, int y, int w, int h, uint32_t c);
    void rrect(Surf &s, int x, int y, int w, int h, int r, uint32_t c);
    void circle(Surf &s, int cx, int cy, int r, uint32_t c);
    void text(Surf &s, int x, int y, int scale, uint32_t c, const char *str);
    int  textw(int scale, const char *str);
    long slen(const char *s);
}

namespace mixtk {

    /* A kernel window the app paints into. Content origin is (0, WM_TITLE_H):
     * the compositor draws the title bar over the top WM_TITLE_H pixels. */
    struct Win {
        int wid, x, y, w, h;
        uint32_t *buf;
    };

    class Widget {
    public:
        int x, y, w, h;              /* content coords (below the title bar) */
        Widget() : x(0), y(0), w(0), h(0) {}
        virtual ~Widget() {}
        virtual void paint(gfx::Surf &s, bool hover, bool pressed) = 0;
        virtual bool on_click(int lx, int ly) { (void)lx; (void)ly; return false; }
        bool hit(int lx, int ly) const {
            return lx >= x && lx < x + w && ly >= y && ly < y + h;
        }
    };

    class Label : public Widget {
    public:
        const char *text;
        uint32_t color;
        int scale;
        Label() : text(""), color(0x333344), scale(1) {}
        void paint(gfx::Surf &s, bool hover, bool pressed) override;
    };

    class Button : public Widget {
    public:
        const char *label;
        uint32_t face, edge, ink;
        void (*on_activate)(void *ctx);
        void *ctx;
        Button() : label(""), face(0x4a7dff), edge(0x2a4daa), ink(0xffffff),
                   on_activate(0), ctx(0) {}
        void paint(gfx::Surf &s, bool hover, bool pressed) override;
        bool on_click(int lx, int ly) override;
    };

    class Toggle : public Widget {
    public:
        const char *name;
        bool value;
        uint32_t ink;
        void (*on_change)(void *ctx, bool value);
        void *ctx;
        Toggle() : name(""), value(false), ink(0x2a2a33), on_change(0), ctx(0) {}
        void paint(gfx::Surf &s, bool hover, bool pressed) override;
        bool on_click(int lx, int ly) override;
    };

    class App {
    public:
        Win win;
        Widget *widgets[12];
        int nwidgets;
        bool dirty;
        App() : nwidgets(0), dirty(true) { win.wid = -1; win.buf = 0; }
        virtual ~App() {}
        void add(Widget *w) { if (nwidgets < 12) widgets[nwidgets++] = w; }
        void attach(int wid, uint32_t *buf, int x, int y, int w, int h);
        gfx::Surf surf();                 /* buffer minus the title bar strip */
        void paint();                     /* content + widgets, then commit */
        bool click(int lx, int ly);       /* dispatch to widgets */
        virtual void paint_content(gfx::Surf &s) { (void)s; }
        virtual bool on_key(char c) { (void)c; return false; }
    };

} // namespace mixtk

#endif
