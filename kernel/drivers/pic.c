#include "pic.h"
#include "lib/io.h"

void pic_init(void)
{
    /* ICW1: init + ICW4 needed */
    outb(PIC1_CMD, 0x11);
    io_wait();
    outb(PIC2_CMD, 0x11);
    io_wait();
    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE);
    io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8);
    io_wait();
    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);   /* slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);
    io_wait();
    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    io_wait();
    outb(PIC2_DATA, 0x01);
    io_wait();
    /* Mask everything for now */
    outb(PIC1_DATA, 0xff);
    outb(PIC2_DATA, 0xff);
}

void pic_set_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq % 8;
    outb(port, inb(port) | (1 << bit));
}

void pic_clear_mask(uint8_t irq)
{
    uint16_t port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = irq % 8;
    outb(port, inb(port) & ~(1 << bit));
}

void pic_eoi(uint8_t vector)
{
    if (vector >= IRQ_BASE + 8)
        outb(PIC2_CMD, 0x20);
    outb(PIC1_CMD, 0x20);
}
