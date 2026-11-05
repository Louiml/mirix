#include "pit.h"
#include "lib/io.h"

void pit_oneshot(uint16_t divisor)
{
    /* channel 0, lobyte/hibyte, mode 0 (interrupt on terminal count), binary */
    outb(PIT_COMMAND, 0x30);
    outb(PIT_CH0_DATA, divisor & 0xff);
    outb(PIT_CH0_DATA, (divisor >> 8) & 0xff);
}

uint16_t pit_read_count(void)
{
    outb(PIT_COMMAND, 0x00);          /* latch channel 0 count */
    uint8_t lo = inb(PIT_CH0_DATA);
    uint8_t hi = inb(PIT_CH0_DATA);
    return (uint16_t)(lo | (hi << 8));
}
