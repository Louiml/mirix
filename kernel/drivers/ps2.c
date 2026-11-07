#include "ps2.h"
#include "drivers/pic.h"
#include "drivers/fb.h"
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

/* ---- key event ring buffer (producer = IRQ, consumer = syscall/main) ----
 * SMP (8c): all rings + cursor position are behind one spinlock (a leaf). */
#include "lib/spinlock.h"
static spinlock_t g_input_lock = SPINLOCK_INIT;

#define KBD_RING 256
static kbd_event_t g_ring[KBD_RING];
static volatile uint32_t g_head, g_tail;

static void kbd_push(const kbd_event_t *ev)   /* IRQ context: IF=0 */
{
    spin_lock(&g_input_lock);
    uint32_t h = g_head;
    g_ring[h % KBD_RING] = *ev;
    g_head = h + 1;
    if (g_head - g_tail > KBD_RING)   /* overflow: drop oldest */
        g_tail = g_head - KBD_RING;
    spin_unlock(&g_input_lock);
}

int kbd_pop(kbd_event_t *out)
{
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    int ok = 0;
    if (g_tail != g_head) {
        *out = g_ring[g_tail % KBD_RING];
        g_tail++;
        ok = 1;
    }
    spin_unlock_irqrestore(&g_input_lock, fl);
    return ok;
}

/* ---- mouse: 3-byte packets on IRQ12, own ring + absolute position ---- */
#define MOUSE_RING 128
static mouse_event_t g_mring[MOUSE_RING];
static volatile uint32_t g_mhead, g_mtail;
static int g_mx, g_my;                 /* absolute cursor position */
static uint8_t g_mbtn;

static void mouse_push(const mouse_event_t *ev)   /* IRQ context: IF=0 */
{
    spin_lock(&g_input_lock);
    uint32_t h = g_mhead;
    g_mring[h % MOUSE_RING] = *ev;
    g_mhead = h + 1;
    if (g_mhead - g_mtail > MOUSE_RING)
        g_mtail = g_mhead - MOUSE_RING;
    spin_unlock(&g_input_lock);
}

int mouse_pop(mouse_event_t *out)
{
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    int ok = 0;
    if (g_mtail != g_mhead) {
        *out = g_mring[g_mtail % MOUSE_RING];
        g_mtail++;
        ok = 1;
    }
    spin_unlock_irqrestore(&g_input_lock, fl);
    return ok;
}

void mouse_get_pos(int *x, int *y, uint8_t *buttons)
{
    uint64_t fl = spin_lock_irqsave(&g_input_lock);
    *x = g_mx;
    *y = g_my;
    *buttons = g_mbtn;
    spin_unlock_irqrestore(&g_input_lock, fl);
}

int input_pop_filtered(input_event_t *out, int filter)
{
    if (filter != 1) {
        kbd_event_t k;
        if (kbd_pop(&k)) {
            out->type = 0;
            out->code = k.code;
            out->pressed = k.pressed;
            out->extended = k.extended;
            out->ascii = k.ascii;
            out->dx = out->dy = 0;
            out->buttons = 0;
            return 1;
        }
    }
    if (filter != 2) {
        mouse_event_t m;
        if (mouse_pop(&m)) {
            out->type = 1;
            out->code = out->pressed = out->extended = 0;
            out->ascii = 0;
            out->dx = m.dx;
            out->dy = m.dy;
            out->buttons = m.buttons;
            return 1;
        }
    }
    return 0;
}

void ps2_mouse_irq(void)
{
    /* AUX output buffer full (status bit 5) and data available (bit 0). */
    if (!(inb(PS2_STATUS) & 0x21) || !(inb(PS2_STATUS) & 0x20))
        return;
    static uint8_t pkt[3];
    static uint8_t idx = 0;
    pkt[idx++] = inb(PS2_DATA);
    if (idx < 3)
        return;
    idx = 0;

    int16_t dx = (int16_t)pkt[1];
    int16_t dy = (int16_t)pkt[2];
    if (pkt[0] & 0x10) dx |= (int16_t)0xff00;   /* X sign bit (9-bit) */
    if (pkt[0] & 0x20) dy |= (int16_t)0xff00;   /* Y sign bit */
    uint8_t buttons = pkt[0] & 0x07;

    /* PS/2 Y axis is inverted vs screen coordinates. */
    mouse_event_t ev = { dx, (int16_t)-dy, buttons };
    mouse_push(&ev);

    spin_lock(&g_input_lock);            /* IRQ context: IF=0 */
    g_mbtn = buttons;
    if (fb_ready()) {
        int nx = g_mx + dx, ny = g_my - dy;
        if (nx < 0) nx = 0;
        if (ny < 0) ny = 0;
        if (nx >= (int)fb_width())  nx = (int)fb_width() - 1;
        if (ny >= (int)fb_height()) ny = (int)fb_height() - 1;
        g_mx = nx;
        g_my = ny;
        fb_set_cursor(g_mx, g_my, 1);
    }
    spin_unlock(&g_input_lock);
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
            /* Phase 9a: 0xE0-extended keys — arrows, home/end, pgup/pgdn, del */
            switch (code) {
            case 0x1c: ev.ascii = '\n';        break;   /* keypad Enter */
            case 0x48: ev.ascii = (char)KEY_UP;    break;
            case 0x50: ev.ascii = (char)KEY_DOWN;  break;
            case 0x4b: ev.ascii = (char)KEY_LEFT;  break;
            case 0x4d: ev.ascii = (char)KEY_RIGHT; break;
            case 0x47: ev.ascii = (char)KEY_HOME;  break;
            case 0x4f: ev.ascii = (char)KEY_END;   break;
            case 0x49: ev.ascii = (char)KEY_PGUP;  break;
            case 0x51: ev.ascii = (char)KEY_PGDN;  break;
            case 0x53: ev.ascii = (char)KEY_DEL;   break;
            default: break;
            }
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

    /* ---- mouse on the aux port (IRQ12) ---- */
    cmd(0xa8);                          /* enable mouse port */
    cmd(0x20);                          /* read config again */
    cfg = data_r();
    cfg |= 0x02;                        /* IRQ12 enable */
    cfg &= ~0x20;                       /* mouse clock enabled */
    cmd(0x60);
    data_w(cfg);
    cmd(0xd4); data_w(0xf4); (void)data_r();   /* enable data reporting, ack */
    cmd(0xd4); data_w(0xf3); data_w(100);      /* sample rate 100/s */
    (void)data_r(); (void)data_r();
    if (fb_ready()) {
        g_mx = (int)fb_width() / 2;
        g_my = (int)fb_height() / 2;
        fb_set_cursor(g_mx, g_my, 1);
    }
    pic_clear_mask(2);                  /* unmask the cascade on the master */
    pic_clear_mask(12);                 /* unmask IRQ12 (slave PIC) */
    uart_printf("ps2: mouse ready (IRQ12, 3-byte packets, reporting on, cfg=%02x)\n", cfg);
}
