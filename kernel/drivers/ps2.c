#include "ps2.h"
#include "drivers/pic.h"
#include "lib/io.h"
#include "lib/uart.h"

#define PS2_DATA    0x60
#define PS2_STATUS  0x64
#define PS2_CMD     0x64

/* Bounded waits: never spin forever if the emulated controller misbehaves. */
static void wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(PS2_STATUS) & 0x02); i++)
        ;
}

static int wait_read(void)
{
    for (int i = 0; i < 100000 && !(inb(PS2_STATUS) & 0x01); i++)
        ;
    return (inb(PS2_STATUS) & 0x01) ? 1 : 0;
}

static void cmd(uint8_t c)  { wait_write(); outb(PS2_CMD, c); }
static void data_w(uint8_t b) { wait_write(); outb(PS2_DATA, b); }
static uint8_t data_r(void) { wait_read(); return inb(PS2_DATA); }

/* ---- key event ring buffer (single producer = IRQ, single consumer) ---- */
#define KBD_RING 256
static kbd_event_t g_ring[KBD_RING];
static volatile uint32_t g_head, g_tail;

static void kbd_push(const kbd_event_t *ev)
{
    uint32_t h = g_head;
    g_ring[h % KBD_RING] = *ev;
    g_head = h + 1;
    if (g_head - g_tail > KBD_RING)   /* overflow: drop oldest */
        g_tail = g_head - KBD_RING;
}

int kbd_pop(kbd_event_t *out)
{
    if (g_tail == g_head)
        return 0;
    *out = g_ring[g_tail % KBD_RING];
    g_tail++;
    return 1;
}

/* ---- scancode set 1 -> ASCII (with shift variants) ---- */
static const char kbd_lower[64] = {
    [0x01] = 27,                          /* ESC */
    [0x02] = '1', [0x03] = '2', [0x04] = '3', [0x05] = '4', [0x06] = '5',
    [0x07] = '6', [0x08] = '7', [0x09] = '8', [0x0a] = '9', [0x0b] = '0',
    [0x0c] = '-', [0x0d] = '=', [0x0e] = '\b', [0x0f] = '\t',
    [0x10] = 'q', [0x11] = 'w', [0x12] = 'e', [0x13] = 'r', [0x14] = 't',
    [0x15] = 'y', [0x16] = 'u', [0x17] = 'i', [0x18] = 'o', [0x19] = 'p',
    [0x1a] = '[', [0x1b] = ']', [0x1c] = '\n', [0x1e] = 'a', [0x1f] = 's',
    [0x20] = 'd', [0x21] = 'f', [0x22] = 'g', [0x23] = 'h', [0x24] = 'j',
    [0x25] = 'k', [0x26] = 'l', [0x27] = ';', [0x28] = '\'', [0x29] = '`',
    [0x2b] = '\\', [0x2c] = 'z', [0x2d] = 'x', [0x2e] = 'c', [0x2f] = 'v',
    [0x30] = 'b', [0x31] = 'n', [0x32] = 'm', [0x33] = ',', [0x34] = '.',
    [0x35] = '/', [0x39] = ' ',
};

static const char kbd_shift[64] = {
    [0x02] = '!', [0x03] = '@', [0x04] = '#', [0x05] = '$', [0x06] = '%',
    [0x07] = '^', [0x08] = '&', [0x09] = '*', [0x0a] = '(', [0x0b] = ')',
    [0x0c] = '_', [0x0d] = '+', [0x0e] = '\b', [0x0f] = '\t',
    [0x10] = 'Q', [0x11] = 'W', [0x12] = 'E', [0x13] = 'R', [0x14] = 'T',
    [0x15] = 'Y', [0x16] = 'U', [0x17] = 'I', [0x18] = 'O', [0x19] = 'P',
    [0x1a] = '{', [0x1b] = '}', [0x1c] = '\n', [0x1e] = 'A', [0x1f] = 'S',
    [0x20] = 'D', [0x21] = 'F', [0x22] = 'G', [0x23] = 'H', [0x24] = 'J',
    [0x25] = 'K', [0x26] = 'L', [0x27] = ':', [0x28] = '"', [0x29] = '~',
    [0x2b] = '|', [0x2c] = 'Z', [0x2d] = 'X', [0x2e] = 'C', [0x2f] = 'V',
    [0x30] = 'B', [0x31] = 'N', [0x32] = 'M', [0x33] = '<', [0x34] = '>',
    [0x35] = '?', [0x39] = ' ',
};

void ps2_keyboard_irq(void)
{
    if (!(inb(PS2_STATUS) & 0x01))
        return;   /* spurious */
    uint8_t sc = inb(PS2_DATA);

    static uint8_t ext = 0;
    static uint8_t lshift = 0, rshift = 0;

    if (sc == 0xe0) {          /* extended prefix */
        ext = 1;
        return;
    }

    uint8_t code = sc & 0x7f;
    int pressed = !(sc & 0x80);
    if (code == 0x2a) lshift = pressed;
    if (code == 0x36) rshift = pressed;

    kbd_event_t ev;
    ev.code = code;
    ev.pressed = (uint8_t)pressed;
    ev.extended = ext;
    ev.ascii = 0;
    if (pressed) {
        if (ext) {
            if (code == 0x1c)          /* keypad Enter */
                ev.ascii = '\n';
        } else if (code < 64) {
            ev.ascii = (lshift || rshift) ? kbd_shift[code] : kbd_lower[code];
        }
    }
    ext = 0;
    kbd_push(&ev);
}

void ps2_init(void)
{
    cmd(0xad);                          /* disable keyboard port */
    cmd(0xa7);                          /* disable mouse port */
    while (inb(PS2_STATUS) & 0x01)      /* flush output buffer */
        (void)inb(PS2_DATA);

    cmd(0x20);                          /* read controller config */
    uint8_t cfg = data_r();
    cfg |= 0x01;                        /* IRQ1 enable */
    cfg |= 0x40;                        /* translation: set2 -> set1 */
    cfg &= ~0x10;                       /* keyboard clock enabled */
    cmd(0x60);
    data_w(cfg);

    cmd(0xae);                          /* enable keyboard port */

    data_w(0xff);                       /* reset keyboard */
    uint8_t ack = data_r();             /* 0xFA */
    uint8_t bat = data_r();             /* 0xAA self-test OK */
    data_w(0xf6);                       /* set defaults */
    (void)data_r();                     /* 0xFA */
    data_w(0xf4);                       /* enable scanning */
    (void)data_r();                     /* 0xFA */

    pic_clear_mask(1);                  /* unmask IRQ1 in the PIC */
    uart_printf("ps2: keyboard ready (IRQ1, scancode set 1, cfg=%02x ack=%02x bat=%02x)\n",
                cfg, ack, bat);
}
