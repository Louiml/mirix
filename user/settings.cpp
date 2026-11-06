/* settings.cpp — System Settings app (Phase 7d), built on mixtk.
 * Toggle rows flip live desktop options (via the aurora_set_* hooks in
 * aurora.c) and persist to /settings.cfg on the ramdisk. */
#include "mixtk.h"
#include "apps.h"

#define SET_FILE "/settings.cfg"

class SettingsApp : public mixtk::App {
public:
    mixtk::Label title;
    mixtk::Toggle t_clock;
    mixtk::Toggle t_dark;
    mixtk::Toggle t_mag;
    bool loaded;

    SettingsApp() : loaded(false) {}

    static void clock_cb(void *ctx, bool v) { aurora_set_clock_seconds(v ? 1 : 0); ((SettingsApp *)ctx)->persist("clock seconds", v); }
    static void dark_cb(void *ctx, bool v)  { aurora_set_term_dark(v ? 1 : 0);   ((SettingsApp *)ctx)->persist("dark terminal", v); }
    static void mag_cb(void *ctx, bool v)   { aurora_set_dock_magnify(v ? 1 : 0);((SettingsApp *)ctx)->persist("dock magnify", v); }

    void persist(const char *name, bool v)
    {
        char m[96]; long n = 0;
        const char *p = "aurora: settings: '";
        while (*p) m[n++] = *p++;
        while (*name) m[n++] = *name++;
        p = "' -> ";
        while (*p) m[n++] = *p++;
        p = v ? "on" : "off";
        while (*p) m[n++] = *p++;
        m[n++] = '\n';
        sys::write(1, m, n);
        save_cfg();
    }

    void save_cfg()
    {
        long fd = sys::open(SET_FILE, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) return;
        char b[64]; long n = 0;
        const char *p;
        p = "clock="; while (*p) b[n++] = *p++;
        b[n++] = (char)('0' + (t_clock.value ? 1 : 0));
        b[n++] = '\n';
        p = "dark="; while (*p) b[n++] = *p++;
        b[n++] = (char)('0' + (t_dark.value ? 1 : 0));
        b[n++] = '\n';
        p = "mag="; while (*p) b[n++] = *p++;
        b[n++] = (char)('0' + (t_mag.value ? 1 : 0));
        b[n++] = '\n';
        sys::write(fd, b, n);
        sys::close(fd);
        char m[80]; long nn = 0;
        p = "aurora: settings: saved " SET_FILE " (";
        while (*p) m[nn++] = *p++;
        { char t[24]; long mm = 0; unsigned long u = (unsigned long)n;
          if (!u) t[mm++] = '0';
          while (u) { t[mm++] = (char)('0' + (u % 10)); u /= 10; }
          while (mm) m[nn++] = t[--mm]; }
        m[nn++] = ')'; m[nn++] = '\n';
        sys::write(1, m, nn);
    }

    void load_cfg()
    {
        long fd = sys::open(SET_FILE, O_RDONLY);
        if (fd < 0) {
            sys::log("aurora: settings: " SET_FILE " not found (defaults)\n");
            return;
        }
        char b[64];
        long n = sys::read(fd, b, (long)sizeof b - 1);
        sys::close(fd);
        if (n < 0) n = 0;
        b[n] = 0;
        for (long i = 0; i + 7 < n; i++) {
            /* lines look like "clock=1\n" / "dark=0\n" / "mag=1\n" */
            if (b[i] == 'c' && b[i+1] == 'l' && b[i+6] == '1') t_clock.value = true;
            if (b[i] == 'd' && b[i+1] == 'a' && b[i+5] == '1') t_dark.value = true;
            if (b[i] == 'm' && b[i+1] == 'a' && b[i+4] == '1') t_mag.value = true;
        }
        /* apply */
        aurora_set_clock_seconds(t_clock.value ? 1 : 0);
        aurora_set_term_dark(t_dark.value ? 1 : 0);
        aurora_set_dock_magnify(t_mag.value ? 1 : 0);
        char m[80]; long nn = 0;
        const char *p = "aurora: settings: loaded " SET_FILE " (";
        while (*p) m[nn++] = *p++;
        { char t[24]; long mm = 0; unsigned long u = (unsigned long)n;
          if (!u) t[mm++] = '0';
          while (u) { t[mm++] = (char)('0' + (u % 10)); u /= 10; }
          while (mm) m[nn++] = t[--mm]; }
        m[nn++] = ')'; m[nn++] = '\n';
        sys::write(1, m, nn);
    }

    void paint_content(gfx::Surf &s) override
    {
        gfx::rect(s, 0, 0, s.w, s.h, 0xf0f1f6);
    }
};

static SettingsApp g_settings;

extern "C" {

void settings_attach(int wid, uint32_t *buf, int x, int y, int w, int h)
{
    g_settings.attach(wid, buf, x, y, w, h);
    if (!g_settings.loaded) {
        g_settings.loaded = true;
        g_settings.title.text = "Appearance";
        g_settings.title.x = 8; g_settings.title.y = 6;
        g_settings.title.color = 0x3a3a44; g_settings.title.scale = 1;
        g_settings.add(&g_settings.title);

        g_settings.t_clock.name = "Clock shows seconds";
        g_settings.t_clock.x = 8; g_settings.t_clock.y = 26;
        g_settings.t_clock.w = w - 16; g_settings.t_clock.h = 24;
        g_settings.t_clock.value = true;
        g_settings.t_clock.on_change = &SettingsApp::clock_cb;
        g_settings.t_clock.ctx = &g_settings;
        g_settings.add(&g_settings.t_clock);

        g_settings.t_dark.name = "Dark terminal";
        g_settings.t_dark.x = 8; g_settings.t_dark.y = 54;
        g_settings.t_dark.w = w - 16; g_settings.t_dark.h = 24;
        g_settings.t_dark.value = true;
        g_settings.t_dark.on_change = &SettingsApp::dark_cb;
        g_settings.t_dark.ctx = &g_settings;
        g_settings.add(&g_settings.t_dark);

        g_settings.t_mag.name = "Dock magnification";
        g_settings.t_mag.x = 8; g_settings.t_mag.y = 82;
        g_settings.t_mag.w = w - 16; g_settings.t_mag.h = 24;
        g_settings.t_mag.value = true;
        g_settings.t_mag.on_change = &SettingsApp::mag_cb;
        g_settings.t_mag.ctx = &g_settings;
        g_settings.add(&g_settings.t_mag);

        g_settings.load_cfg();   /* may flip the toggles + apply */
        sys::log("aurora: settings: open\n");
    }
    g_settings.dirty = true;
    settings_paint();
}

void settings_detach(void) { g_settings.win.wid = -1; g_settings.dirty = false; }

int settings_click(int lx, int ly)
{
    if (g_settings.win.wid < 0) return 0;
    if (g_settings.click(lx, ly)) { settings_paint(); return 1; }
    return 0;
}

int settings_dirty(void) { return g_settings.dirty ? 1 : 0; }

void settings_paint(void)
{
    if (g_settings.win.wid >= 0) g_settings.paint();
}

} // extern "C"
