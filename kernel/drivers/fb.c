#include "fb.h"
#include "lib/string.h"
#include "lib/printf.h"
#include "mm/kmalloc.h"

#include <stdarg.h>

/* Public-domain 8x8 bitmap font (IBM VGA derivative, see
 * assets/fonts/LICENSE-font8x8.txt). One byte per row, MSB = leftmost. */
#include "assets/fonts/font8x8_basic.h"

static struct limine_framebuffer *g_fb;
static uint32_t g_w, g_h, g_pitch, g_bpp, g_bytespp;
static uint8_t g_rsize, g_rshift, g_gsize, g_gshift, g_bsize, g_bshift;
static int g_ok;

static uint32_t g_cur_x, g_cur_y;
static uint32_t g_fg = 0xffffff;
static int g_mirror = 1;   /* uart_putc mirrors to the fb console when set */

/* Phase 6a: back buffer (allocated once the heap exists). All drawing goes
 * to g_draw (back buffer if present, else the front buffer directly). */
static uint32_t *g_back;
static uint32_t *g_draw;
static int g_dirty;

/* Phase 6b: mouse cursor, composited onto the front buffer at present time
 * (never into the back buffer, so it doesn't smear). */
static int g_cur_on;
static int g_mx, g_my;

/* Scale an 8-bit channel into a mask of `size` bits. */
static uint32_t scale_channel(uint32_t v8, uint32_t size)
{
    uint32_t max = (1u << size) - 1;
    return (v8 * max + 127) / 255;
}

static uint32_t pack(uint32_t rgb)
{
    uint32_t r = (rgb >> 16) & 0xff, g = (rgb >> 8) & 0xff, b = rgb & 0xff;
    return (scale_channel(r, g_rsize) << g_rshift)
         | (scale_channel(g, g_gsize) << g_gshift)
         | (scale_channel(b, g_bsize) << g_bshift);
}

void fb_init(struct limine_framebuffer *fb)
{
    g_fb = fb;
    g_w = fb->width;
    g_h = fb->height;
    g_pitch = fb->pitch;
    g_bpp = fb->bpp;
    g_bytespp = (g_bpp + 7) / 8;
    g_rsize = fb->red_mask_size;   g_rshift = fb->red_mask_shift;
    g_gsize = fb->green_mask_size; g_gshift = fb->green_mask_shift;
    g_bsize = fb->blue_mask_size;  g_bshift = fb->blue_mask_shift;
    g_ok = (fb->memory_model == LIMINE_FRAMEBUFFER_RGB && g_bytespp <= 4);
    g_cur_x = 0;
    g_cur_y = FB_HEADER_PX;   /* text area starts below the status strip */
    g_back = 0;
    g_draw = (uint32_t *)fb->address;   /* single-buffered until heap is up */
    g_dirty = 0;
}

int fb_ready(void) { return g_ok; }
uint32_t fb_width(void) { return g_w; }
uint32_t fb_height(void) { return g_h; }

void fb_putpixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (!g_ok || x >= g_w || y >= g_h)
        return;
    uint32_t px = pack(rgb);
    uint8_t *dst = (uint8_t *)g_draw + y * g_pitch + x * g_bytespp;
    for (uint32_t i = 0; i < g_bytespp; i++)   /* little-endian store */
        dst[i] = (uint8_t)(px >> (8 * i));
    g_dirty = 1;
}

void fb_clear(uint32_t rgb)
{
    if (!g_ok)
        return;
    uint32_t px = pack(rgb);
    for (uint32_t y = 0; y < g_h; y++) {
        uint8_t *row = (uint8_t *)g_draw + y * g_pitch;
        for (uint32_t x = 0; x < g_w; x++) {
            for (uint32_t i = 0; i < g_bytespp; i++)
                row[x * g_bytespp + i] = (uint8_t)(px >> (8 * i));
        }
    }
    g_cur_x = 0;
    g_cur_y = FB_HEADER_PX;
    g_dirty = 1;
}

void fb_draw_header(void)
{
    if (!g_ok)
        return;
    static const uint32_t bars[8] = {
        0xff0000, 0xff8800, 0xffff00, 0x00ff00,
        0x00ffff, 0x0088ff, 0x8800ff, 0xffffff,
    };
    uint32_t bw = g_w / 8;
    for (int i = 0; i < 8; i++)
        for (uint32_t y = 0; y < FB_HEADER_PX; y++)
            for (uint32_t x = (uint32_t)i * bw;
                 x < (uint32_t)(i + 1) * bw && x < g_w; x++)
                fb_putpixel(x, y, bars[i]);
}

void fb_text(uint32_t x, uint32_t y, uint8_t scale, uint32_t rgb, const char *s)
{
    if (!g_ok)
        return;
    for (const char *p = s; *p; p++) {
        const char *glyph = font8x8_basic[(unsigned char)*p & 0x7f];
        for (int gy = 0; gy < 8; gy++) {
            for (int gx = 0; gx < 8; gx++) {
                if ((glyph[gy] >> (7 - gx)) & 1) {
                    for (uint32_t dy = 0; dy < scale; dy++)
                        for (uint32_t dx = 0; dx < scale; dx++)
                            fb_putpixel(x + (uint32_t)gx * scale + dx,
                                        y + (uint32_t)gy * scale + dy, rgb);
                }
            }
        }
        x += 8 * scale;
    }
}

/* Scroll the text area (below the header strip) up by one line. Called only
 * when the cursor would run past the bottom, so the header is preserved. */
static void fb_scroll(void)
{
    uint32_t line = 8 * FB_SCALE;
    uint32_t top = FB_HEADER_PX;
    if (g_cur_y + line <= g_h)
        return;
    uint8_t *fb = (uint8_t *)g_draw;
    memmove(fb + top * g_pitch,
            fb + (top + line) * g_pitch,
            (size_t)(g_h - top - line) * g_pitch);
    /* clear the bottom line of the text area */
    uint32_t bg = pack(0x0d0d18);
    for (uint32_t y = g_h - line; y < g_h; y++) {
        uint8_t *row = fb + y * g_pitch;
        for (uint32_t x = 0; x < g_w; x++)
            for (uint32_t i = 0; i < g_bytespp; i++)
                row[x * g_bytespp + i] = (uint8_t)(bg >> (8 * i));
    }
    g_cur_y -= line;
    g_dirty = 1;
}

void fb_putc(char c)
{
    if (!g_ok)
        return;
    if (c == '\n') {
        g_cur_x = 0;
        g_cur_y += 8 * FB_SCALE;
    } else if (c == '\r') {
        g_cur_x = 0;
    } else {
        if (g_cur_x + 8 * FB_SCALE > g_w) {
            g_cur_x = 0;
            g_cur_y += 8 * FB_SCALE;
        }
        char s[2] = { c, 0 };
        fb_text(g_cur_x, g_cur_y, FB_SCALE, g_fg, s);
        g_cur_x += 8 * FB_SCALE;
    }
    if (g_cur_y + 8 * FB_SCALE > g_h)
        fb_scroll();
}

void fb_puts(const char *s)
{
    while (*s)
        fb_putc(*s++);
}

static void fb_sink(void *ctx, char c)
{
    (void)ctx;
    fb_putc(c);
}

void fb_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fb_sink, 0, fmt, ap);
    va_end(ap);
}

void fb_set_mirror(int on) { g_mirror = on; }
int  fb_mirror(void)        { return g_mirror; }

/* ---- Phase 6c: pixel read + alpha blend (for the TrueType renderer) ---- */

uint32_t fb_getpixel(uint32_t x, uint32_t y)
{
    if (!g_ok || x >= g_w || y >= g_h)
        return 0;
    uint8_t *src = (uint8_t *)g_draw + y * g_pitch + x * g_bytespp;
    uint32_t px = 0;
    for (uint32_t i = 0; i < g_bytespp; i++)
        px |= (uint32_t)src[i] << (8 * i);
    uint32_t r = (px >> g_rshift) & ((1u << g_rsize) - 1);
    uint32_t g = (px >> g_gshift) & ((1u << g_gsize) - 1);
    uint32_t b = (px >> g_bshift) & ((1u << g_bsize) - 1);
    /* normalize each channel to 8 bits */
    if (g_rsize && g_rsize < 8) r = (r * 255) / ((1u << g_rsize) - 1);
    if (g_gsize && g_gsize < 8) g = (g * 255) / ((1u << g_gsize) - 1);
    if (g_bsize && g_bsize < 8) b = (b * 255) / ((1u << g_bsize) - 1);
    if (g_rsize > 8) r >>= (g_rsize - 8);
    if (g_gsize > 8) g >>= (g_gsize - 8);
    if (g_bsize > 8) b >>= (g_bsize - 8);
    return (r << 16) | (g << 8) | b;
}

void fb_blendpixel(uint32_t x, uint32_t y, uint32_t rgb, uint8_t alpha)
{
    if (!g_ok || x >= g_w || y >= g_h || alpha == 0)
        return;
    if (alpha == 255) {
        fb_putpixel(x, y, rgb);
        return;
    }
    uint32_t dst = fb_getpixel(x, y);
    uint32_t dr = (dst >> 16) & 0xff, dg = (dst >> 8) & 0xff, db = dst & 0xff;
    uint32_t sr = (rgb >> 16) & 0xff, sg = (rgb >> 8) & 0xff, sb = rgb & 0xff;
    uint32_t ia = 255 - alpha;
    uint32_t r = (sr * alpha + dr * ia) / 255;
    uint32_t g = (sg * alpha + dg * ia) / 255;
    uint32_t b = (sb * alpha + db * ia) / 255;
    fb_putpixel(x, y, (r << 16) | (g << 8) | b);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb)
{
    if (!g_ok)
        return;
    if (x >= g_w) return;
    if (y >= g_h) return;
    if (x + w > g_w) w = g_w - x;
    if (y + h > g_h) h = g_h - y;
    uint32_t px = pack(rgb);
    for (uint32_t yy = 0; yy < h; yy++) {
        uint8_t *row = (uint8_t *)g_draw + (y + yy) * g_pitch + x * g_bytespp;
        for (uint32_t xx = 0; xx < w; xx++)
            for (uint32_t i = 0; i < g_bytespp; i++)
                row[xx * g_bytespp + i] = (uint8_t)(px >> (8 * i));
    }
    g_dirty = 1;
}

void fb_copy_rect(uint32_t dx, uint32_t dy, const void *src, uint32_t src_pitch,
                  uint32_t w, uint32_t h)
{
    if (!g_ok || !src)
        return;
    if (dx >= g_w || dy >= g_h)
        return;
    if (dx + w > g_w) w = g_w - dx;
    if (dy + h > g_h) h = g_h - dy;
    for (uint32_t yy = 0; yy < h; yy++)
        memcpy((uint8_t *)g_draw + (dy + yy) * g_pitch + dx * g_bytespp,
               (const uint8_t *)src + yy * src_pitch,
               (size_t)w * g_bytespp);
    g_dirty = 1;
}

/* ---- Phase 6a: double buffering ---- */

void fb_enable_double_buffer(void)
{
    if (!g_ok || g_back)
        return;
    size_t bytes = (size_t)g_w * g_h * g_bytespp;
    g_back = kmalloc(bytes);
    if (!g_back) {
        g_draw = (uint32_t *)g_fb->address;
        return;
    }
    /* start the back buffer as a copy of the current front */
    memcpy(g_back, g_fb->address, bytes);
    g_draw = g_back;
    g_dirty = 1;
}

int fb_dirty(void) { return g_dirty; }

/* Simple 12x19 arrow cursor bitmap (1 = white, 2 = black outline). */
static const uint8_t cursor_bmp[19][12] = {
    {2,0,0,0,0,0,0,0,0,0,0,0},
    {2,2,0,0,0,0,0,0,0,0,0,0},
    {2,1,2,0,0,0,0,0,0,0,0,0},
    {2,1,1,2,0,0,0,0,0,0,0,0},
    {2,1,1,1,2,0,0,0,0,0,0,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,1,1,1,2,0,0,0,0,0},
    {2,1,1,1,1,1,1,2,0,0,0,0},
    {2,1,1,1,1,1,1,1,2,0,0,0},
    {2,1,1,1,1,1,1,1,1,2,0,0},
    {2,1,1,1,1,1,2,2,2,2,2,0},
    {2,1,1,1,1,2,0,0,0,0,0,0},
    {2,1,1,2,1,1,2,0,0,0,0,0},
    {2,1,2,0,2,1,1,2,0,0,0,0},
    {2,2,0,0,2,1,1,2,0,0,0,0},
    {0,0,0,0,2,1,1,2,0,0,0,0},
    {0,0,0,0,2,1,1,1,2,0,0,0},
    {0,0,0,0,0,2,1,1,1,2,0,0},
    {0,0,0,0,0,2,2,2,2,2,2,0},
};

void fb_set_cursor(int x, int y, int visible)
{
    if (g_mx != x || g_my != y || g_cur_on != visible)
        g_dirty = 1;   /* front must be re-copied so the old cursor erases */
    g_mx = x;
    g_my = y;
    g_cur_on = visible;
}

static void blit_front_pixel(uint32_t x, uint32_t y, uint32_t rgb)
{
    if (x >= g_w || y >= g_h)
        return;
    uint32_t px = pack(rgb);
    uint8_t *dst = (uint8_t *)g_fb->address + y * g_pitch + x * g_bytespp;
    for (uint32_t i = 0; i < g_bytespp; i++)
        dst[i] = (uint8_t)(px >> (8 * i));
}

/* Draw the cursor straight onto the front buffer (over the freshly copied
 * back buffer). Called only from fb_present, so it never accumulates. */
void fb_draw_cursor(void)
{
    if (!g_ok || !g_cur_on)
        return;
    for (int gy = 0; gy < 19; gy++)
        for (int gx = 0; gx < 12; gx++) {
            uint8_t v = cursor_bmp[gy][gx];
            if (!v)
                continue;
            blit_front_pixel((uint32_t)g_mx + gx, (uint32_t)g_my + gy,
                             v == 1 ? 0xffffff : 0x000000);
        }
}

void fb_present(void)
{
    if (!g_ok || !g_back)
        return;
    if (!g_dirty)
        return;
    size_t bytes = (size_t)g_w * g_h * g_bytespp;
    memcpy(g_fb->address, g_back, bytes);
    g_dirty = 0;
    fb_draw_cursor();
}
