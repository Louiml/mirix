/* Framebuffer console: draws text onto the linear framebuffer Limine gave
 * us. Colors are 0xRRGGBB; they are packed through the framebuffer's own
 * channel masks, so any RGB layout works. */
#ifndef MIRIX_DRIVERS_FB_H
#define MIRIX_DRIVERS_FB_H

#include <stdint.h>
#include "limine.h"

void fb_init(struct limine_framebuffer *fb);
int  fb_ready(void);
uint32_t fb_width(void);
uint32_t fb_height(void);

void fb_clear(uint32_t rgb);
void fb_putpixel(uint32_t x, uint32_t y, uint32_t rgb);
/* Phase 6c (TrueType AA): read a pixel as 0xRRGGBB and alpha-blend onto the
 * back buffer (src-over). */
uint32_t fb_getpixel(uint32_t x, uint32_t y);
void fb_blendpixel(uint32_t x, uint32_t y, uint32_t rgb, uint8_t alpha);

/* Phase 7 (compositor): fast opaque ops onto the back buffer. */
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t rgb);
/* Copy a raw 32-bpp block (src_pitch in bytes) into the back buffer. */
void fb_copy_rect(uint32_t dx, uint32_t dy, const void *src, uint32_t src_pitch,
                  uint32_t w, uint32_t h);
/* Draw a string at pixel (x, y), each glyph scaled `scale` times. */
void fb_text(uint32_t x, uint32_t y, uint8_t scale, uint32_t rgb, const char *s);

/* Scrolling console at a cursor (glyph size 8x8 * FB_SCALE). */
#define FB_SCALE 2
void fb_putc(char c);
void fb_puts(const char *s);
void fb_printf(const char *fmt, ...);

/* Top status strip with 8 color blocks; the text area starts below it and
 * scrolling preserves it. (Also keeps headless screendumps colorful.) */
#define FB_HEADER_PX 16
void fb_draw_header(void);

/* Phase 6a: double buffering. All drawing goes to a back buffer; fb_present()
 * copies it to the visible framebuffer. Call fb_enable_double_buffer() once
 * the heap is up (before that, drawing goes straight to the front buffer). */
void fb_enable_double_buffer(void);
void fb_present(void);          /* back -> front, if dirty */
int  fb_dirty(void);
void fb_set_cursor(int x, int y, int visible);   /* 6b: mouse cursor */
void fb_draw_cursor(void);                        /* composited on present */

/* Serial->screen mirror: when on, uart_putc also renders each char to the
 * scrolling fb console. Panic paths turn it off so the red screen stays clean. */
void fb_set_mirror(int on);
int  fb_mirror(void);

#endif
