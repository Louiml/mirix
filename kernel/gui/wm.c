/* Aurora compositor — desktop background, menu bar, window frames
 * (rounded corners, drop shadow, traffic lights, TrueType title) and the
 * per-window content blit. Everything renders into the fb back buffer;
 * the main loop presents it. */
#include "wm.h"
#include "drivers/fb.h"
#include "lib/ttf.h"
#include "lib/string.h"
#include "lib/uart.h"
#include "mm/pmm.h"
#include "mm/mm.h"
#include "mm/kmalloc.h"

static wm_win_t g_wins[WM_MAX_WINS];
static int g_z[WM_MAX_WINS];          /* indices, back to front */
static int g_nz;
static int g_dirty = 1;
static uint32_t *g_bg;                /* cached desktop background (packed px) */

void wm_init(void)
{
    memset(g_wins, 0, sizeof(g_wins));
    g_nz = 0;
    g_dirty = 1;
    if (fb_ready() && !g_bg) {
        g_bg = kmalloc((size_t)fb_width() * fb_height() * 4);
        if (g_bg) {
            /* vertical gradient: deep indigo -> near-black, plus 3 translucent
             * diagonal "aurora" bands (green/teal/violet) */
            uint32_t W = fb_width(), H = fb_height();
            for (uint32_t y = 0; y < H; y++) {
                uint32_t t = (y * 255) / (H > 1 ? H - 1 : 1);
                uint32_t r = 24 + (t * 10) / 255;
                uint32_t g = 22 + (t * 6) / 255;
                uint32_t b = 52 + (t * 30) / 255;
                uint32_t row = (r << 16) | (g << 8) | b;
                for (uint32_t x = 0; x < W; x++)
                    g_bg[y * W + x] = row;
            }
            for (int band = 0; band < 3; band++) {
                uint32_t col[3] = { 0x30ffa0, 0x40d0ff, 0xb060ff };
                for (uint32_t y = 0; y < H; y++)
                    for (uint32_t x = 0; x < W; x++) {
                        /* band center moves diagonally; width ~90 px */
                        int32_t cx = (int32_t)(W / 4) + band * (int32_t)(W / 5) +
                                     (int32_t)(y / 3);
                        int32_t d = (int32_t)x - cx;
                        if (d < 0) d = -d;
                        if (d < 90) {
                            uint32_t a = (uint32_t)(90 - d) * 40 / 90; /* 0..40 */
                            uint32_t px = g_bg[y * W + x];
                            uint32_t pr = (px >> 16) & 0xff, pg = (px >> 8) & 0xff,
                                     pb = px & 0xff;
                            uint32_t sr = (col[band] >> 16) & 0xff,
                                     sg = (col[band] >> 8) & 0xff,
                                     sb = col[band] & 0xff;
                            pr = (sr * a + pr * (255 - a)) / 255;
                            pg = (sg * a + pg * (255 - a)) / 255;
                            pb = (sb * a + pb * (255 - a)) / 255;
                            g_bg[y * W + x] = (pr << 16) | (pg << 8) | pb;
                        }
                    }
            }
        }
    }
}

int wm_count(void) { return g_nz; }
const wm_win_t *wm_wins(void) { return g_wins; }
int wm_dirty(void) { return g_dirty; }

void wm_commit(int wid)
{
    if (wid >= 0 && wid < WM_MAX_WINS && g_wins[wid].used)
        g_dirty = 1;
    else
        g_dirty = 1;
}

int wm_create(int x, int y, int w, int h, const char *title, int flags)
{
    if (w <= 0 || h <= 0 || w > WM_WIN_MAX_DIM || h > WM_WIN_MAX_DIM)
        return -1;
    int wid = -1;
    for (int i = 0; i < WM_MAX_WINS; i++)
        if (!g_wins[i].used) { wid = i; break; }
    if (wid < 0)
        return -1;
    uint64_t phys = pmm_alloc_pages_contig(WIN_BUF_PAGES);
    if (!phys)
        return -1;
    wm_win_t *wn = &g_wins[wid];
    memset(wn, 0, sizeof(*wn));
    wn->used = 1;
    wn->x = x; wn->y = y; wn->w = w; wn->h = h;
    wn->flags = flags;
    wn->visible = 1;
    wn->focused = 0;
    wn->buf_phys = phys;
    wn->buf = phys_to_virt(phys);
    memset(wn->buf, 0, WIN_BUF_PAGES * PAGE_SIZE);
    if (title) {
        strncpy(wn->title, title, sizeof(wn->title) - 1);
        wn->title[sizeof(wn->title) - 1] = 0;
    }
    /* append to z (front) */
    g_z[g_nz++] = wid;
    g_dirty = 1;
    return wid;
}

void wm_close(int wid)
{
    if (wid < 0 || wid >= WM_MAX_WINS || !g_wins[wid].used)
        return;
    for (uint64_t p = g_wins[wid].buf_phys;
         p < g_wins[wid].buf_phys + WIN_BUF_PAGES * PAGE_SIZE; p += PAGE_SIZE)
        pmm_free_page(p);
    memset(&g_wins[wid], 0, sizeof(wm_win_t));
    for (int i = 0; i < g_nz; i++)
        if (g_z[i] == wid) {
            for (int j = i; j + 1 < g_nz; j++)
                g_z[j] = g_z[j + 1];
            g_nz--;
            break;
        }
    g_dirty = 1;
}

void wm_move(int wid, int x, int y)
{
    if (wid < 0 || wid >= WM_MAX_WINS || !g_wins[wid].used)
        return;
    g_wins[wid].x = x;
    g_wins[wid].y = y;
    g_dirty = 1;
}

void wm_resize(int wid, int w, int h)
{
    if (wid < 0 || wid >= WM_MAX_WINS || !g_wins[wid].used)
        return;
    if (w < 40) w = 40;
    if (h < 40) h = 40;
    if (w > WM_WIN_MAX_DIM) w = WM_WIN_MAX_DIM;
    if (h > WM_WIN_MAX_DIM) h = WM_WIN_MAX_DIM;
    g_wins[wid].w = w;
    g_wins[wid].h = h;
    g_dirty = 1;
}

void wm_focus(int wid)
{
    if (wid < 0 || wid >= WM_MAX_WINS || !g_wins[wid].used)
        return;
    for (int i = 0; i < WM_MAX_WINS; i++)
        g_wins[i].focused = 0;
    g_wins[wid].focused = 1;
    /* raise to front */
    int pos = -1;
    for (int i = 0; i < g_nz; i++)
        if (g_z[i] == wid) { pos = i; break; }
    if (pos >= 0 && pos != g_nz - 1) {
        for (int i = pos; i + 1 < g_nz; i++)
            g_z[i] = g_z[i + 1];
        g_z[g_nz - 1] = wid;
    }
    g_dirty = 1;
}

int wm_top_at(int x, int y)
{
    for (int i = g_nz - 1; i >= 0; i--) {
        wm_win_t *wn = &g_wins[g_z[i]];
        if (!wn->used || !wn->visible)
            continue;
        if (x >= wn->x && x < wn->x + wn->w && y >= wn->y && y < wn->y + wn->h)
            return g_z[i];
    }
    return -1;
}

/* ---------------- drawing helpers (back buffer) ------------------------- */

static void fill_round(int x, int y, int w, int h, int r, uint32_t rgb,
                       uint8_t alpha)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            /* rounded-corner mask */
            int dx = 0, dy = 0;
            if (xx < r && yy < r) { dx = r - xx; dy = r - yy; }
            else if (xx >= w - r && yy < r) { dx = xx - (w - r - 1); dy = r - yy; }
            else if (xx < r && yy >= h - r) { dx = r - xx; dy = yy - (h - r - 1); }
            else if (xx >= w - r && yy >= h - r) { dx = xx - (w - r - 1); dy = yy - (h - r - 1); }
            if (dx * dx + dy * dy > r * r)
                continue;
            fb_blendpixel((uint32_t)(x + xx), (uint32_t)(y + yy), rgb, alpha);
        }
}

static void circle(int cx, int cy, int r, uint32_t rgb)
{
    for (int yy = -r; yy <= r; yy++)
        for (int xx = -r; xx <= r; xx++)
            if (xx * xx + yy * yy <= r * r)
                fb_putpixel((uint32_t)(cx + xx), (uint32_t)(cy + yy), rgb);
}

/* ---------------- full composite ---------------------------------------- */

void wm_render(void)
{
    if (!fb_ready())
        return;
    uint32_t W = fb_width(), H = fb_height();

    /* background */
    if (g_bg)
        fb_copy_rect(0, 0, g_bg, W * 4, W, H);
    else
        fb_fill_rect(0, 0, W, H, 0x141626);

    /* menu bar (top strip, translucent dark) */
    fb_fill_rect(0, 0, W, WM_MENU_H, 0x101018);
    for (uint32_t x = 0; x < W; x++)
        fb_blendpixel(x, WM_MENU_H, 0x8888ff, 90);
    if (ttf_ready()) {
        ttf_draw_text(10, 5, 15, 0xffffff, "Aurora");
        const char *right = "MirixOS";
        int rw = ttf_text_width(right, 13);
        ttf_draw_text((int)(W - rw - 12), 7, 13, 0xccccdd, right);
    }

    /* windows, back to front */
    for (int zi = 0; zi < g_nz; zi++) {
        wm_win_t *wn = &g_wins[g_z[zi]];
        if (!wn->used || !wn->visible)
            continue;
        int x = wn->x, y = wn->y, w = wn->w, h = wn->h;
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        int frameless = (wn->flags & WM_FRAMELESS);
        int th = frameless ? 0 : WM_TITLE_H;

        /* drop shadow (two soft passes) */
        fill_round(x - 6, y - 2 + 8, w + 12, h + 14, WM_RADIUS + 4, 0x000000, 60);
        fill_round(x - 3, y - 1 + 4, w + 6, h + 8, WM_RADIUS + 2, 0x000000, 50);

        /* body */
        fill_round(x, y, w, h, WM_RADIUS, 0xf4f4f8, 245);
        /* title bar (rounded top over the body) */
        if (!frameless) {
            fill_round(x, y, w, th, WM_RADIUS, 0xd8d8e0, 255);
            fb_fill_rect((uint32_t)x, (uint32_t)(y + th - WM_RADIUS),
                         (uint32_t)w, (uint32_t)WM_RADIUS, 0xd8d8e0);
            /* traffic lights */
            circle(x + 14, y + th / 2, 6, 0xff5f57);   /* close */
            circle(x + 34, y + th / 2, 6, 0xfebc2e);   /* minimize */
            circle(x + 54, y + th / 2, 6, 0x28c840);   /* zoom */
            /* title text, centered */
            if (ttf_ready() && wn->title[0]) {
                int tw = ttf_text_width(wn->title, 13);
                int tx = x + (w - tw) / 2;
                if (tx < x + 70) tx = x + 70;
                ttf_draw_text(tx, y + 7, 13, 0x3a3a44, wn->title);
            }
        }
        /* content */
        int cw = w, ch = h - th;
        if (cw > 0 && ch > 0 && wn->buf) {
            /* blit the window buffer, honoring the rounded corners */
            int r = frameless ? WM_RADIUS : WM_RADIUS;
            for (int yy = 0; yy < ch; yy++) {
                for (int xx = 0; xx < cw; xx++) {
                    int dx = 0, dy = 0;
                    int in_corner = 0;
                    if (xx < r && yy + th < r) { dx = r - xx; dy = r - (yy + th); in_corner = 1; }
                    else if (xx >= cw - r && yy + th < r) { dx = xx - (cw - r - 1); dy = r - (yy + th); in_corner = 1; }
                    else if (xx < r && yy + th >= h - r) { dx = r - xx; dy = (yy + th) - (h - r - 1); in_corner = 1; }
                    else if (xx >= cw - r && yy + th >= h - r) { dx = xx - (cw - r - 1); dy = (yy + th) - (h - r - 1); in_corner = 1; }
                    if (in_corner && dx * dx + dy * dy > r * r)
                        continue;
                    uint32_t px = ((const uint32_t *)wn->buf)[yy * cw + xx];
                    fb_putpixel((uint32_t)(x + xx), (uint32_t)(y + th + yy), px);
                }
            }
        }
        /* focus ring */
        if (wn->focused) {
            for (int xx = 0; xx < w; xx++) {
                fb_blendpixel((uint32_t)(x + xx), (uint32_t)y, 0x6aa8ff, 200);
                fb_blendpixel((uint32_t)(x + xx), (uint32_t)(y + h - 1), 0x6aa8ff, 200);
            }
            for (int yy = 0; yy < h; yy++) {
                fb_blendpixel((uint32_t)x, (uint32_t)(y + yy), 0x6aa8ff, 200);
                fb_blendpixel((uint32_t)(x + w - 1), (uint32_t)(y + yy), 0x6aa8ff, 200);
            }
        }
    }
    g_dirty = 0;
}
