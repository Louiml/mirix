/* PS/2 controller + keyboard (i8042, ports 0x60/0x64), IRQ1, scancode set 1.
 * The IRQ handler decodes make/break codes into key events on a ring buffer;
 * consumers poll with kbd_pop(). */
#ifndef MIRIX_DRIVERS_PS2_H
#define MIRIX_DRIVERS_PS2_H

#include <stdint.h>

typedef struct {
    uint8_t code;      /* scancode (0x7f-masked) */
    uint8_t pressed;   /* 1 = make, 0 = break */
    uint8_t extended;  /* 0xE0-prefixed */
    char    ascii;     /* printable mapping, or 0 */
} kbd_event_t;

void ps2_init(void);
void ps2_keyboard_irq(void);          /* called from idt.c on IRQ1 */
int  kbd_pop(kbd_event_t *out);       /* 1 if an event was popped */

#endif
