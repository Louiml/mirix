/* Minimal TrueType font renderer (glyf outlines, cmap format 4, composite
 * glyphs) with a glyph cache and 4x horizontal antialiasing. All math is
 * 16.16 fixed point in int32/int64 — no FPU needed. Rendering blends 8-bit
 * alpha glyphs onto the framebuffer back buffer. */
#ifndef MIRIX_LIB_TTF_H
#define MIRIX_LIB_TTF_H

#include <stdint.h>
#include <stddef.h>

int  ttf_load(const uint8_t *data, size_t len);   /* 0 = ok */
int  ttf_ready(void);
uint32_t ttf_upm(void);
uint32_t ttf_num_glyphs(void);

int  ttf_text_width(const char *s, int px);        /* advance width in px */
/* Draw a string with its line-box top at (x, y). Blends onto the fb. */
void ttf_draw_text(int x, int y, int px, uint32_t rgb, const char *s);

#endif
