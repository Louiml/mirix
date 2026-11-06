/* editor.cpp — Text Editor app (Phase 7d), built on the mixtk toolkit.
 * Linked into the aurora binary; aurora's input loop forwards keystrokes and
 * content clicks here while the Editor window has keyboard focus.
 * The document is /notes.txt on the ramdisk; it autosaves on every edit. */
#include "mixtk.h"
#include "apps.h"

#define ED_ROWS 30
#define ED_COLS 60
#define ED_FILE "/notes.txt"
#define ED_TEXT_Y 34          /* content y where line 0 starts */

static void logf2(const char *a, const char *b)   /* a + b + '\n' */
{
    char m[128]; long n = 0;
    while (*a) m[n++] = *a++;
    while (*b) m[n++] = *b++;
    m[n++] = '\n';
    sys::write(1, m, n);
}
static long logdec(char *m, long n, long v)
{
    char t[24]; long mm = 0;
    unsigned long u = v < 0 ? (unsigned long)(-v) : (unsigned long)v;
    if (v < 0) m[n++] = '-';
    if (!u) t[mm++] = '0';
    while (u) { t[mm++] = (char)('0' + (u % 10)); u /= 10; }
    while (mm) m[n++] = t[--mm];
    return n;
}

class EditorApp : public mixtk::App {
public:
    char lines[ED_ROWS][ED_COLS + 1];
    int nrows;
    int crow, ccol;
    char pending[ED_COLS + 1];
    int npending;
    bool loaded;
    mixtk::Label filelbl;
    mixtk::Button savebtn;

    EditorApp() : nrows(1), crow(0), ccol(0), npending(0), loaded(false) {}

    static void save_cb(void *ctx) { ((EditorApp *)ctx)->save(); }

    void blank_line(int r)
    {
        for (int c = 0; c < ED_COLS; c++) lines[r][c] = ' ';
        lines[r][ED_COLS] = 0;
    }

    void load()
    {
        for (int r = 0; r < ED_ROWS; r++) blank_line(r);
        nrows = 1; crow = 0; ccol = 0; npending = 0;
        long fd = sys::open(ED_FILE, O_RDONLY);
        if (fd < 0) {
            logf2("aurora: editor: " ED_FILE " not found (new document)", "");
        } else {
            char buf[ED_ROWS * (ED_COLS + 1) + 1];
            long n = sys::read(fd, buf, (long)sizeof buf - 1);
            sys::close(fd);
            if (n < 0) n = 0;
            buf[n] = 0;
            int r = 0, c = 0;
            for (long i = 0; i < n && r < ED_ROWS; i++) {
                char ch = buf[i];
                if (ch == '\n') {
                    lines[r][c] = 0;
                    if (++r < ED_ROWS) { blank_line(r); c = 0; }
                    continue;
                }
                if (ch == '\r') continue;
                if (c < ED_COLS) lines[r][c++] = ch;
            }
            lines[r][c] = 0;
            nrows = r + 1;
            crow = r; ccol = c;
            char m[96]; long nn = 0;
            const char *p = "aurora: editor: loaded " ED_FILE " (";
            while (*p) m[nn++] = *p++;
            nn = logdec(m, nn, n);
            m[nn++] = ')'; m[nn++] = '\n';
            sys::write(1, m, nn);
        }
        loaded = true;
    }

    long doc_bytes()
    {
        long total = 0;
        for (int r = 0; r < nrows; r++) {
            long l = 0;
            while (lines[r][l] && l < ED_COLS) l++;
            while (l > 0 && lines[r][l - 1] == ' ') l--;
            total += l + 1;   /* + '\n' */
        }
        return total;
    }

    void save()
    {
        long fd = sys::open(ED_FILE, O_CREAT | O_WRONLY | O_TRUNC);
        if (fd < 0) {
            logf2("aurora: editor: save FAILED (open)", "");
            return;
        }
        for (int r = 0; r < nrows; r++) {
            long l = 0;
            while (lines[r][l] && l < ED_COLS) l++;
            while (l > 0 && lines[r][l - 1] == ' ') l--;
            if (l) sys::write(fd, lines[r], l);
            sys::write(fd, "\n", 1);
        }
        sys::close(fd);
        char m[96]; long n = 0;
        const char *p = "aurora: editor: saved " ED_FILE " (";
        while (*p) m[n++] = *p++;
        n = logdec(m, n, doc_bytes());
        m[n++] = ')'; m[n++] = '\n';
        sys::write(1, m, n);
    }

    void changed() { dirty = true; save(); }   /* autosave on every edit */

    bool on_key(char c) override
    {
        if (c == '\n') {
            pending[npending] = 0;
            char m[96]; long n = 0;
            const char *p = "aurora: editor: line '";
            while (*p) m[n++] = *p++;
            for (int i = 0; i < npending && n < 80; i++) m[n++] = pending[i];
            m[n++] = '\''; m[n++] = '\n';
            sys::write(1, m, n);
            npending = 0;
            if (crow + 1 < ED_ROWS) {
                /* split: tail of the current line becomes a new line below */
                for (int r = ED_ROWS - 1; r > crow + 1; r--)
                    for (int cc = 0; cc <= ED_COLS; cc++) lines[r][cc] = lines[r - 1][cc];
                for (int cc = 0; cc <= ED_COLS; cc++)
                    lines[crow + 1][cc] = (ccol + cc <= ED_COLS) ? lines[crow][ccol + cc] : 0;
                lines[crow][ccol] = 0;
                for (int cc = ccol; cc < ED_COLS; cc++) lines[crow][cc] = ' ';
                lines[crow][ED_COLS] = 0;
                crow++; ccol = 0;
                if (crow + 1 > nrows) nrows = crow + 1;
            }
            changed();
            return true;
        }
        if (c == '\b') {
            if (ccol > 0) {
                for (int cc = ccol - 1; cc < ED_COLS - 1; cc++)
                    lines[crow][cc] = lines[crow][cc + 1];
                lines[crow][ED_COLS - 1] = ' ';
                ccol--;
                if (npending > 0) npending--;
            } else if (crow > 0) {
                int pl = 0;
                while (lines[crow - 1][pl] && pl < ED_COLS) pl++;
                int mv = 0;
                while (lines[crow][mv] && pl + mv < ED_COLS) {
                    lines[crow - 1][pl + mv] = lines[crow][mv];
                    mv++;
                }
                lines[crow - 1][pl + mv] = 0;
                crow--; ccol = pl;
                for (int r = crow + 1; r + 1 < nrows; r++)
                    for (int cc = 0; cc <= ED_COLS; cc++) lines[r][cc] = lines[r + 1][cc];
                if (nrows > 1) nrows--;
                blank_line(nrows < ED_ROWS ? nrows : ED_ROWS - 1);
            }
            changed();
            return true;
        }
        if (c < 32 || c >= 127) return false;
        if (ccol >= ED_COLS) { changed(); return true; }  /* line full */
        for (int cc = ED_COLS - 1; cc > ccol; cc--)
            lines[crow][cc] = lines[crow][cc - 1];
        lines[crow][ccol] = c;
        ccol++;
        if (npending < ED_COLS) pending[npending++] = c;
        changed();
        return true;
    }

    bool click_content(int lx, int ly)
    {
        if (ly < ED_TEXT_Y) return false;
        int col = lx / 8;
        int row = (ly - ED_TEXT_Y) / 8;
        if (row >= ED_ROWS) row = ED_ROWS - 1;
        if (col >= ED_COLS) col = ED_COLS - 1;
        if (col < 0) col = 0;
        if (row < 0) row = 0;
        crow = row; ccol = col;
        char m[64]; long n = 0;
        const char *p = "aurora: editor: click -> row ";
        while (*p) m[n++] = *p++;
        n = logdec(m, n, crow);
        p = " col ";
        while (*p) m[n++] = *p++;
        n = logdec(m, n, ccol);
        m[n++] = '\n';
        sys::write(1, m, n);
        dirty = true;
        return true;
    }

    void paint_content(gfx::Surf &s) override
    {
        gfx::rect(s, 0, 0, s.w, s.h, 0xffffff);
        for (int r = 0; r < nrows && r < ED_ROWS; r++) {
            int yy = ED_TEXT_Y + r * 8;
            if (yy + 8 > s.h) break;
            char line[ED_COLS + 1];
            for (int c = 0; c < ED_COLS; c++) line[c] = lines[r][c];
            line[ED_COLS] = 0;
            gfx::text(s, 6, yy, 1, r == crow ? 0x1a1a2a : 0x777788, line);
        }
        int cx = 6 + ccol * 8, cy = ED_TEXT_Y + crow * 8;
        if (cy + 8 <= s.h)
            gfx::rect(s, cx, cy, 8, 8, 0x40d0ff);
    }
};

static EditorApp g_editor;

extern "C" {

void editor_attach(int wid, uint32_t *buf, int x, int y, int w, int h)
{
    g_editor.attach(wid, buf, x, y, w, h);
    if (!g_editor.loaded) {
        g_editor.load();
        g_editor.filelbl.text = ED_FILE;
        g_editor.filelbl.x = 8; g_editor.filelbl.y = 8;
        g_editor.filelbl.color = 0x555566;
        g_editor.add(&g_editor.filelbl);
        g_editor.savebtn.label = "Save";
        g_editor.savebtn.x = w - 84; g_editor.savebtn.y = 5;
        g_editor.savebtn.w = 76; g_editor.savebtn.h = 20;
        g_editor.savebtn.on_activate = &EditorApp::save_cb;
        g_editor.savebtn.ctx = &g_editor;
        g_editor.add(&g_editor.savebtn);
        logf2("aurora: editor: open", "");
    }
    g_editor.dirty = true;
    editor_paint();
}

void editor_detach(void) { g_editor.win.wid = -1; g_editor.dirty = false; }

int editor_key(char c)
{
    if (g_editor.win.wid < 0) return 0;
    return g_editor.on_key(c) ? 1 : 0;
}

int editor_click(int lx, int ly)
{
    if (g_editor.win.wid < 0) return 0;
    if (g_editor.click(lx, ly)) { editor_paint(); return 1; }
    if (g_editor.click_content(lx, ly)) { editor_paint(); return 1; }
    return 0;
}

int editor_dirty(void) { return g_editor.dirty ? 1 : 0; }

void editor_paint(void)
{
    if (g_editor.win.wid >= 0) g_editor.paint();
}

} // extern "C"
