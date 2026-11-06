/* Minimal TrueType renderer for MirixOS (Phase 6c).
 *
 * Parses the sfnt container (head/maxp/hhea/hmtx/loca/glyf/cmap), resolves
 * codepoints through cmap format 4, loads simple + composite glyf outlines,
 * flattens quadratic beziers, and rasterizes with a nonzero-winding scanline
 * fill at 4x horizontal subsampling for antialiasing (8-bit alpha bitmaps).
 *
 * ALL math is integer 16.16 fixed point (int32/int64) — the kernel is built
 * with -mno-sse/-mno-mmx, so no floating point anywhere. Device coordinates
 * are 16.16 px relative to the glyph origin (pen position at the baseline,
 * y pointing DOWN after the font's y-up flip).
 *
 * Rendered glyphs go into a small LRU cache keyed by (codepoint, px) so
 * repeated text (console redraws, UI labels) is cheap.
 */
#include "ttf.h"
#include "drivers/fb.h"
#include "lib/string.h"
#include "mm/kmalloc.h"

/* ---------------- sfnt access (bounds-checked, little-endian) ---------- */

static const uint8_t *g_data;
static size_t g_len;
static int g_ready;

static uint32_t t_head, t_maxp, t_hhea, t_hmtx, t_loca, t_glyf, t_cmap_sub;
static uint32_t g_upm;
static uint32_t g_num_glyphs;
static int32_t  g_ascender, g_linegap;
static uint32_t g_num_hmetrics;
static int16_t  g_index_to_loc_format;

/* TrueType/sfnt is BIG-ENDIAN — all multi-byte fields read MSB first. */
static uint16_t rd16(size_t o)
{
    if (o + 2 > g_len) return 0;
    return (uint16_t)(((uint16_t)g_data[o] << 8) | g_data[o + 1]);
}
static int16_t rds16(size_t o) { return (int16_t)rd16(o); }
static uint32_t rd32(size_t o)
{
    if (o + 4 > g_len) return 0;
    return ((uint32_t)g_data[o] << 24) | ((uint32_t)g_data[o + 1] << 16) |
           ((uint32_t)g_data[o + 2] << 8) | (uint32_t)g_data[o + 3];
}

/* ---------------- cmap format 4 lookup -------------------------------- */

static uint32_t cmap_lookup(uint32_t cp)
{
    if (!g_ready || cp > 0xFFFF || t_cmap_sub == 0)
        return 0;
    size_t sub = t_cmap_sub;
    if (rd16(sub) != 4)
        return 0;
    uint32_t segX2 = rd16(sub + 6);
    int seg = (int)(segX2 / 2);
    if (seg <= 0 || seg > 4096)
        return 0;
    size_t end_off   = sub + 14;
    size_t start_off = sub + 16 + segX2;
    size_t delta_off = sub + 16 + 2 * segX2;
    size_t range_off = sub + 16 + 3 * segX2;
    /* binary search: first segment with endCode >= cp */
    int lo = 0, hi = seg - 1, i = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (rd16(end_off + 2 * (size_t)mid) >= cp) { i = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    if (i < 0)
        return 0;
    uint32_t start = rd16(start_off + 2 * (size_t)i);
    if (cp < start)
        return 0;
    int16_t delta = rds16(delta_off + 2 * (size_t)i);
    uint32_t range = rd16(range_off + 2 * (size_t)i);
    if (range == 0)
        return (uint32_t)((cp + delta) & 0xFFFF);
    /* idRangeOffset is relative to its own slot */
    size_t addr = range_off + 2 * (size_t)i + range + 2 * (size_t)(cp - start);
    uint32_t gi = rd16(addr);
    if (gi == 0)
        return 0;
    return (uint32_t)((gi + delta) & 0xFFFF);
}

/* ---------------- glyf outline collection ----------------------------- */

#define MAX_PTS   4096
#define MAX_EDGES 16384
#define MAX_CONT  128

typedef struct {
    int32_t x, y;      /* font units */
    uint8_t on;
} fpt_t;

typedef struct {
    int32_t x0, y0, x1, y1;   /* device 16.16 */
    int dir;                  /* +1 if y1>y0 else -1 */
} edge_t;

static fpt_t  g_pts[MAX_PTS];
static uint16_t g_ends[MAX_CONT];
static int g_ncont, g_npts;

static edge_t g_edges[MAX_EDGES];
static int g_nedges;

/* 2.14 fixed-point affine transform + translation (font units) */
typedef struct {
    int32_t m00, m01, m10, m11;   /* 2.14 */
    int32_t dx, dy;               /* font units */
} xform_t;

static void xform_apply(const xform_t *m, int32_t fx, int32_t fy,
                        int32_t *ox, int32_t *oy)
{
    *ox = (int32_t)(((int64_t)m->m00 * fx + (int64_t)m->m10 * fy) >> 14) + m->dx;
    *oy = (int32_t)(((int64_t)m->m01 * fx + (int64_t)m->m11 * fy) >> 14) + m->dy;
}

static void xform_compose(const xform_t *outer, const xform_t *inner, xform_t *out)
{
    out->m00 = (int32_t)(((int64_t)outer->m00 * inner->m00 + (int64_t)outer->m10 * inner->m01) >> 14);
    out->m01 = (int32_t)(((int64_t)outer->m01 * inner->m00 + (int64_t)outer->m11 * inner->m01) >> 14);
    out->m10 = (int32_t)(((int64_t)outer->m00 * inner->m10 + (int64_t)outer->m10 * inner->m11) >> 14);
    out->m11 = (int32_t)(((int64_t)outer->m01 * inner->m10 + (int64_t)outer->m11 * inner->m11) >> 14);
    int32_t tx, ty;
    xform_apply(outer, inner->dx, inner->dy, &tx, &ty);
    out->dx = tx;
    out->dy = ty;
}

/* Parse one glyf entry (font units, already offset into the glyf table),
 * appending transformed points to the global arrays. Returns 0 on success. */
static int collect_glyph(uint32_t gi, const xform_t *xf, int depth)
{
    if (depth > 8 || gi >= g_num_glyphs)
        return -1;
    uint32_t off;
    if (g_index_to_loc_format == 0)
        off = (uint32_t)rd16(t_loca + 2 * (size_t)gi) * 2;
    else
        off = rd32(t_loca + 4 * (size_t)gi);
    uint32_t off2;
    if (g_index_to_loc_format == 0)
        off2 = (uint32_t)rd16(t_loca + 2 * (size_t)(gi + 1)) * 2;
    else
        off2 = rd32(t_loca + 4 * (size_t)(gi + 1));
    if (off2 <= off)
        return 0;   /* empty glyph (e.g. space) */
    size_t g = t_glyf + off;
    if (g + 10 > g_len)
        return -1;
    int ncont = rds16(g);
    if (ncont >= 0) {
        /* ---- simple glyph ---- */
        if (ncont > MAX_CONT)
            return -1;
        if (g_npts + (int)(rd16(g + 10 + 2 * (size_t)(ncont - 1)) + 1) > MAX_PTS)
            return -1;
        for (int i = 0; i < ncont; i++)
            g_ends[g_ncont + i] = (uint16_t)(g_npts + rd16(g + 10 + 2 * (size_t)i));
        g_ncont += ncont;
        size_t pos = g + 10 + 2 * (size_t)ncont;
        if (pos + 2 > g_len)
            return -1;
        uint32_t instr_len = rd16(pos);
        pos += 2 + instr_len;
        /* flags */
        int total = g_ends[g_ncont - 1] - (g_ncont > 1 ? g_ends[g_ncont - 2] : 0);
        /* recompute total points of this glyph */
        int base = g_npts;
        total = rd16(g + 10 + 2 * (size_t)(ncont - 1)) + 1;
        uint8_t flags[MAX_PTS];
        int nf = 0;
        while (nf < total) {
            if (pos >= g_len)
                return -1;
            uint8_t f = g_data[pos++];
            flags[nf++] = f;
            if (f & 0x08) {   /* REPEAT */
                if (pos >= g_len)
                    return -1;
                uint8_t rep = g_data[pos++];
                for (int r = 0; r < rep && nf < total; r++)
                    flags[nf++] = f;
            }
        }
        /* x coordinates (delta-encoded) */
        int32_t x = 0;
        for (int i = 0; i < total; i++) {
            uint8_t f = flags[i];
            if (f & 0x02) {           /* X_SHORT_VECTOR */
                if (pos >= g_len) return -1;
                uint8_t v = g_data[pos++];
                x += (f & 0x10) ? v : -v;
            } else if (!(f & 0x10)) { /* X same -> int16 delta */
                if (pos + 2 > g_len) return -1;
                x += rds16(pos);
                pos += 2;
            }
            int32_t ox, oy_unused;
            xform_apply(xf, x, 0, &ox, &oy_unused);
            g_pts[g_npts + i].x = ox;
            g_pts[g_npts + i].on = (uint8_t)(f & 0x01);
        }
        /* y coordinates */
        int32_t y = 0;
        for (int i = 0; i < total; i++) {
            uint8_t f = flags[i];
            if (f & 0x04) {           /* Y_SHORT_VECTOR */
                if (pos >= g_len) return -1;
                uint8_t v = g_data[pos++];
                y += (f & 0x20) ? v : -v;
            } else if (!(f & 0x20)) {
                if (pos + 2 > g_len) return -1;
                y += rds16(pos);
                pos += 2;
            }
            int32_t ox, oy;
            xform_apply(xf, 0, y, &ox, &oy);
            g_pts[g_npts + i].y = oy;
        }
        g_npts += total;
        return 0;
    }
    /* ---- composite glyph ---- */
    size_t pos = g + 10;
    for (;;) {
        if (pos + 4 > g_len)
            return -1;
        uint16_t flags = rd16(pos);
        uint16_t cgi   = rd16(pos + 2);
        pos += 4;
        xform_t comp;
        comp.m00 = 1 << 14; comp.m01 = 0; comp.m10 = 0; comp.m11 = 1 << 14;
        comp.dx = 0; comp.dy = 0;
        if (flags & 0x0001) {          /* ARG_1_AND_2_ARE_WORDS */
            if (pos + 4 > g_len) return -1;
            if (flags & 0x0002) {      /* ARGS_ARE_XY_VALUES */
                comp.dx = rds16(pos);
                comp.dy = rds16(pos + 2);
            }
            pos += 4;
        } else {
            if (pos + 2 > g_len) return -1;
            if (flags & 0x0002) {
                comp.dx = (int8_t)g_data[pos];
                comp.dy = (int8_t)g_data[pos + 1];
            }
            pos += 2;
        }
        if (flags & 0x0008) {          /* WE_HAVE_A_SCALE */
            if (pos + 2 > g_len) return -1;
            comp.m00 = comp.m11 = rds16(pos);
            pos += 2;
        } else if (flags & 0x0040) {   /* X_AND_Y_SCALE */
            if (pos + 4 > g_len) return -1;
            comp.m00 = rds16(pos);
            comp.m11 = rds16(pos + 2);
            pos += 4;
        } else if (flags & 0x0080) {   /* TWO_BY_TWO */
            if (pos + 8 > g_len) return -1;
            comp.m00 = rds16(pos);
            comp.m01 = rds16(pos + 2);
            comp.m10 = rds16(pos + 4);
            comp.m11 = rds16(pos + 6);
            pos += 8;
        }
        xform_t total_xf;
        xform_compose(xf, &comp, &total_xf);
        if (collect_glyph(cgi, &total_xf, depth + 1) != 0)
            return -1;
        if (!(flags & 0x0020))        /* MORE_COMPONENTS */
            break;
    }
    return 0;
}

/* ---------------- outline -> device edges (flatten quads) ------------- */

/* g_scale = px * 65536 / upm, so device16 = font_units * g_scale (no shift:
 * the <<16 is already inside g_scale). y is flipped (font y-up -> screen down). */
static int64_t g_scale;

static int32_t to_dev_x(int32_t fx) { return (int32_t)((int64_t)fx * g_scale); }
static int32_t to_dev_y(int32_t fy) { return (int32_t)(-((int64_t)fy * g_scale)); }

static void emit_edge(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1)
        return;
    if (g_nedges >= MAX_EDGES)
        return;
    g_edges[g_nedges].x0 = x0;
    g_edges[g_nedges].y0 = y0;
    g_edges[g_nedges].x1 = x1;
    g_edges[g_nedges].y1 = y1;
    g_edges[g_nedges].dir = (y1 > y0) ? 1 : -1;
    g_nedges++;
}

/* Flatten one quadratic bezier (p0, c, p1) in device16, emitting lines. */
static void emit_quad(int32_t x0, int32_t y0, int32_t cx, int32_t cy,
                      int32_t x1, int32_t y1)
{
    const int N = 16;
    int32_t px = x0, py = y0;
    for (int i = 1; i <= N; i++) {
        int64_t t = (int64_t)i * 65536 / N;      /* 16.16 */
        int64_t u = 65536 - t;
        int64_t a = u * u, b = 2 * u * t, c = t * t;
        int64_t qx = (a * x0 + b * cx + c * x1) >> 32;
        int64_t qy = (a * y0 + b * cy + c * y1) >> 32;
        emit_edge(px, py, (int32_t)qx, (int32_t)qy);
        px = (int32_t)qx;
        py = (int32_t)qy;
    }
}

static void flatten_contour(int s, int e)
{
    /* device-space copies of the contour points */
    static int32_t qx[MAX_PTS], qy[MAX_PTS];
    static uint8_t qon[MAX_PTS];
    int n = e - s + 1;
    if (n <= 0)
        return;
    int first_on = -1;
    for (int i = 0; i < n; i++)
        if (g_pts[s + i].on) { first_on = i; break; }

    int cnt = 0;
    if (first_on < 0) {
        /* all off-curve: implied on-curve midpoint between last and first */
        qx[cnt] = (to_dev_x(g_pts[s + n - 1].x) + to_dev_x(g_pts[s].x)) / 2;
        qy[cnt] = (to_dev_y(g_pts[s + n - 1].y) + to_dev_y(g_pts[s].y)) / 2;
        qon[cnt] = 1;
        cnt++;
        for (int i = 0; i < n; i++) {
            qx[cnt] = to_dev_x(g_pts[s + i].x);
            qy[cnt] = to_dev_y(g_pts[s + i].y);
            qon[cnt] = 0;
            cnt++;
        }
    } else {
        for (int i = 0; i < n; i++) {
            int j = (first_on + i) % n;
            qx[cnt] = to_dev_x(g_pts[s + j].x);
            qy[cnt] = to_dev_y(g_pts[s + j].y);
            qon[cnt] = g_pts[s + j].on;
            cnt++;
        }
    }

    int32_t curx = qx[0], cury = qy[0];
    int have_ctrl = 0;
    int32_t ctrlx = 0, ctrly = 0;
    for (int k = 1; k <= cnt; k++) {   /* q[cnt] wraps to q[0] (on-curve) */
        int32_t px, py;
        uint8_t pon;
        if (k < cnt) { px = qx[k]; py = qy[k]; pon = qon[k]; }
        else         { px = qx[0]; py = qy[0]; pon = 1; }
        if (pon) {
            if (have_ctrl)
                emit_quad(curx, cury, ctrlx, ctrly, px, py);
            else
                emit_edge(curx, cury, px, py);
            curx = px; cury = py;
            have_ctrl = 0;
        } else {
            if (have_ctrl) {
                /* two consecutive off-curve points: implied on-curve midpoint */
                int32_t mx = (ctrlx + px) / 2, my = (ctrly + py) / 2;
                emit_quad(curx, cury, ctrlx, ctrly, mx, my);
                curx = mx; cury = my;
            }
            ctrlx = px; ctrly = py;
            have_ctrl = 1;
        }
    }
}

/* ---------------- rasterizer (nonzero winding, 4x horizontal AA) ------- */

typedef struct { int32_t x; int dir; } cross_t;

/* Render glyph gi at scale S (16.16). Fills *w,*h,*xoff,*yoff and returns a
 * kmalloc'd w*h alpha bitmap (caller frees), or NULL for empty glyphs. */
static uint8_t *render_glyph(uint32_t gi, int64_t S,
                             int *w, int *h, int *xoff, int *yoff)
{
    g_ncont = 0;
    g_npts = 0;
    g_nedges = 0;
    g_scale = S;

    xform_t id = { 1 << 14, 0, 0, 1 << 14, 0, 0 };
    if (collect_glyph(gi, &id, 0) != 0)
        return NULL;
    if (g_npts == 0)
        return NULL;

    int s = 0;
    for (int c = 0; c < g_ncont; c++) {
        flatten_contour(s, g_ends[c]);
        s = g_ends[c] + 1;
    }
    if (g_nedges == 0)
        return NULL;

    /* bbox over all edge endpoints */
    int32_t minx = g_edges[0].x0, maxx = g_edges[0].x0;
    int32_t miny = g_edges[0].y0, maxy = g_edges[0].y0;
    for (int i = 0; i < g_nedges; i++) {
        int32_t xs[2] = { g_edges[i].x0, g_edges[i].x1 };
        int32_t ys[2] = { g_edges[i].y0, g_edges[i].y1 };
        for (int k = 0; k < 2; k++) {
            if (xs[k] < minx) minx = xs[k];
            if (xs[k] > maxx) maxx = xs[k];
            if (ys[k] < miny) miny = ys[k];
            if (ys[k] > maxy) maxy = ys[k];
        }
    }
    int gw = (int)((maxx - minx) >> 16) + 2;
    int gh = (int)((maxy - miny) >> 16) + 2;
    if (gw <= 0 || gh <= 0 || gw > 512 || gh > 512)
        return NULL;

    uint8_t *bmp = kmalloc((size_t)gw * gh);
    if (!bmp)
        return NULL;
    memset(bmp, 0, (size_t)gw * gh);

    static cross_t cross[MAX_EDGES];
    for (int py = 0; py < gh; py++) {
        int32_t yc = miny + ((int32_t)py << 16) + 32768;   /* scanline center */
        int nx = 0;
        for (int i = 0; i < g_nedges; i++) {
            int32_t y0 = g_edges[i].y0, y1 = g_edges[i].y1;
            /* half-open [min,max) to avoid double-counting shared vertices */
            if ((y0 <= yc && y1 > yc) || (y1 <= yc && y0 > yc)) {
                int64_t t = ((int64_t)(yc - y0) << 16) / (y1 - y0);
                int32_t xi = g_edges[i].x0 +
                             (int32_t)(((int64_t)(g_edges[i].x1 - g_edges[i].x0) * t) >> 16);
                cross[nx].x = xi;
                cross[nx].dir = g_edges[i].dir;
                nx++;
            }
        }
        /* insertion sort by x (nx is small) */
        for (int a = 1; a < nx; a++) {
            cross_t c = cross[a];
            int b = a - 1;
            while (b >= 0 && cross[b].x > c.x) { cross[b + 1] = cross[b]; b--; }
            cross[b + 1] = c;
        }
        for (int px = 0; px < gw; px++) {
            int cov = 0;
            for (int k = 0; k < 4; k++) {
                int32_t xq = minx + (px << 18) + (k << 16) + 8192; /* subsample center */
                int wind = 0;
                for (int c = 0; c < nx; c++)
                    if (cross[c].x >= xq)
                        wind += cross[c].dir;
                if (wind != 0)
                    cov++;
            }
            bmp[py * gw + px] = (uint8_t)(cov * 255 / 4);
        }
    }

    *w = gw;
    *h = gh;
    *xoff = (int)(minx >> 16);
    *yoff = (int)(miny >> 16);
    return bmp;
}

/* ---------------- glyph cache ----------------------------------------- */

#define TTF_CACHE 192
typedef struct {
    uint32_t cp;
    int px;
    int w, h, xoff, yoff;
    uint8_t *bmp;
    uint32_t stamp;
} cent_t;
static cent_t g_cache[TTF_CACHE];
static uint32_t g_stamp;

static cent_t *cache_get(uint32_t cp, int px, int64_t S)
{
    for (int i = 0; i < TTF_CACHE; i++)
        if (g_cache[i].bmp && g_cache[i].cp == cp && g_cache[i].px == px) {
            g_cache[i].stamp = ++g_stamp;
            return &g_cache[i];
        }
    /* miss: render into the stalest slot */
    int slot = 0;
    for (int i = 1; i < TTF_CACHE; i++)
        if (g_cache[i].stamp < g_cache[slot].stamp)
            slot = i;
    cent_t *e = &g_cache[slot];
    uint32_t gi = cmap_lookup(cp);
    if (gi == 0 && cp != 0)
        gi = cmap_lookup('?');           /* fallback glyph */
    int w, h, xo, yo;
    uint8_t *bmp = render_glyph(gi, S, &w, &h, &xo, &yo);
    if (e->bmp)
        kfree(e->bmp);
    e->cp = cp;
    e->px = px;
    e->bmp = bmp;
    e->w = w; e->h = h; e->xoff = xo; e->yoff = yo;
    e->stamp = ++g_stamp;
    if (!bmp) { e->w = e->h = 0; e->xoff = e->yoff = 0; }
    return e;
}

static int advance_px(uint32_t gi, int64_t S)
{
    uint32_t adv;
    if (gi < g_num_hmetrics)
        adv = rd16(t_hmtx + 4 * (size_t)gi);
    else
        adv = rd16(t_hmtx + 4 * (size_t)(g_num_hmetrics - 1));
    /* adv * S is the advance in 16.16 px; round to nearest whole pixel */
    return (int)(((int64_t)adv * S + 32768) >> 16);
}

/* ---------------- UTF-8 ------------------------------------------------ */

static uint32_t utf8_next(const char **sp)
{
    const uint8_t *s = (const uint8_t *)*sp;
    uint32_t c = s[0];
    if (c == 0) { return 0; }
    if (c < 0x80) { *sp += 1; return c; }
    if ((c & 0xE0) == 0xC0 && s[1]) {
        *sp += 2;
        return ((c & 0x1F) << 6) | (s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && s[1] && s[2]) {
        *sp += 3;
        return ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && s[1] && s[2] && s[3]) {
        *sp += 4;
        return ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
               ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    }
    *sp += 1;
    return 0xFFFD;
}

/* ---------------- public API ------------------------------------------- */

int ttf_load(const uint8_t *data, size_t len)
{
    if (!data || len < 12)
        return -1;
    g_data = data;
    g_len = len;
    uint32_t magic = rd32(0);
    if (magic != 0x00010000 && magic != 0x74727565 /* 'true' */)
        return -2;
    uint32_t ntables = rd16(4);
    uint32_t head = 0, maxp = 0, hhea = 0, hmtx = 0, loca = 0, glyf = 0, cmap = 0;
    for (uint32_t i = 0; i < ntables && 12 + 16 * (i + 1) <= len; i++) {
        size_t rec = 12 + 16 * i;
        uint32_t tag = rd32(rec);
        uint32_t off = rd32(rec + 8);
        switch (tag) {
        case 0x68656164: head = off; break;   /* 'head' */
        case 0x6d617870: maxp = off; break;   /* 'maxp' */
        case 0x68686561: hhea = off; break;   /* 'hhea' */
        case 0x686d7478: hmtx = off; break;   /* 'hmtx' */
        case 0x6c6f6361: loca = off; break;   /* 'loca' */
        case 0x676c7966: glyf = off; break;   /* 'glyf' */
        case 0x636d6170: cmap = off; break;   /* 'cmap' */
        }
    }
    if (!head || !maxp || !hhea || !hmtx || !loca || !glyf || !cmap)
        return -3;
    t_head = head; t_maxp = maxp; t_hhea = hhea; t_hmtx = hmtx;
    t_loca = loca; t_glyf = glyf;

    g_upm = rd16(head + 18);
    g_index_to_loc_format = rds16(head + 50);
    g_num_glyphs = rd16(maxp + 4);
    g_ascender = rds16(hhea + 4);
    g_linegap = rds16(hhea + 8);
    g_num_hmetrics = rd16(hhea + 34);
    if (g_upm == 0 || g_num_glyphs == 0 || g_num_hmetrics == 0)
        return -4;

    /* pick a cmap subtable: prefer (3,1) or (0,x), any format 4 */
    t_cmap_sub = 0;
    uint32_t nsub = rd16(cmap + 2);
    uint32_t best = 0;
    for (uint32_t i = 0; i < nsub && cmap + 4 + 8 * (i + 1) <= len; i++) {
        size_t rec = cmap + 4 + 8 * i;
        uint32_t plat = rd16(rec);
        uint32_t enc = rd16(rec + 2);
        uint32_t off = rd32(rec + 4);
        if (cmap + off + 2 > len)
            continue;
        if (rd16(cmap + off) != 4)
            continue;
        if (plat == 3 && enc == 1) { t_cmap_sub = cmap + off; break; }
        if (plat == 0 && best == 0)
            best = cmap + off;
    }
    if (t_cmap_sub == 0)
        t_cmap_sub = best;
    if (t_cmap_sub == 0)
        return -5;

    memset(g_cache, 0, sizeof(g_cache));
    g_stamp = 0;
    g_ready = 1;
    return 0;
}

int ttf_ready(void) { return g_ready; }
uint32_t ttf_upm(void) { return g_upm; }
uint32_t ttf_num_glyphs(void) { return g_num_glyphs; }

int ttf_text_width(const char *s, int px)
{
    if (!g_ready || !s)
        return 0;
    int64_t S = ((int64_t)px << 16) / g_upm;
    int w = 0;
    const char *p = s;
    uint32_t cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp == '\n')
            continue;
        w += advance_px(cmap_lookup(cp), S);
    }
    return w;
}

void ttf_draw_text(int x, int y, int px, uint32_t rgb, const char *s)
{
    if (!g_ready || !s || !fb_ready() || px <= 0)
        return;
    int64_t S = ((int64_t)px << 16) / g_upm;
    int baseline = y + (int)(((int64_t)g_ascender * S) >> 16);
    int lineh = (int)((((int64_t)(g_ascender - rds16(t_hhea + 6) + g_linegap)) * S) >> 16);
    if (lineh <= 0)
        lineh = px;
    int pen = x;
    const char *p = s;
    uint32_t cp;
    while ((cp = utf8_next(&p)) != 0) {
        if (cp == '\n') {
            pen = x;
            baseline += lineh;
            continue;
        }
        uint32_t gi = cmap_lookup(cp);
        cent_t *e = cache_get(cp, px, S);
        if (e->bmp) {
            for (int gy = 0; gy < e->h; gy++)
                for (int gx = 0; gx < e->w; gx++) {
                    uint8_t a = e->bmp[gy * e->w + gx];
                    if (a)
                        fb_blendpixel((uint32_t)(pen + e->xoff + gx),
                                      (uint32_t)(baseline + e->yoff + gy),
                                      rgb, a);
                }
        }
        pen += advance_px(gi, S);
    }
}
