/* PS/2 controller + keyboard (i8042, ports 0x60/0x64), IRQ1, scancode set 1,
 * plus the PS/2 mouse on IRQ12 (3-byte packets). IRQ handlers push events on
 * ring buffers; consumers poll with kbd_pop()/mouse_pop()/input_pop(). */
#ifndef MIRIX_DRIVERS_PS2_H
#define MIRIX_DRIVERS_PS2_H

#include <stdint.h>

typedef struct {
    uint8_t code;      /* scancode (0x7f-masked) */
    uint8_t pressed;   /* 1 = make, 0 = break */
    uint8_t extended;  /* 0xE0-prefixed */
    char    ascii;     /* printable mapping, or 0 */
} kbd_event_t;

typedef struct {
    int16_t dx, dy;    /* relative movement this packet */
    uint8_t buttons;   /* bit0 left, bit1 right, bit2 middle */
} mouse_event_t;

/* Unified input event for SYS_getevent (type 0 = kbd, 1 = mouse). */
typedef struct {
    uint8_t type;
    uint8_t code, pressed, extended;
    char    ascii;
    int16_t dx, dy;
    uint8_t buttons;
} input_event_t;

/* Phase 9a: special (non-printable) key codes delivered in .ascii for
 * 0xE0-extended scancodes — arrows, home/end, pgup/pgdn, delete. Consumers
 * compare as (unsigned char). */
#define KEY_UP    0x81
#define KEY_DOWN  0x82
#define KEY_LEFT  0x83
#define KEY_RIGHT 0x84
#define KEY_HOME  0x85
#define KEY_END   0x86
#define KEY_PGUP  0x87
#define KEY_PGDN  0x88
#define KEY_DEL   0x89

void ps2_init(void);
void ps2_keyboard_irq(void);          /* called from idt.c on IRQ1 */
void ps2_mouse_irq(void);             /* called from idt.c on IRQ12 */
int  kbd_pop(kbd_event_t *out);       /* 1 if an event was popped */
int  mouse_pop(mouse_event_t *out);
/* Pop one event. filter: 0 = any, 1 = mouse only, 2 = keyboard only.
 * (Aurora passes 1 so it never steals the shell's keystrokes.) */
int  input_pop_filtered(input_event_t *out, int filter);
void mouse_get_pos(int *x, int *y, uint8_t *buttons);

#endif
