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
/* Draw a string at pixel (x, y), each glyph scaled `scale` times. */
void fb_text(uint32_t x, uint32_t y, uint8_t scale, uint32_t rgb, const char *s);

/* Scrolling console at a cursor (glyph size 8x8 * FB_SCALE). */
#define FB_SCALE 2
void fb_putc(char c);
void fb_puts(const char *s);
void fb_printf(const char *fmt, ...);

#endif
