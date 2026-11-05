/* 8259 PIC: remapped to vectors 0x20-0x2f so they don't collide with CPU
 * exceptions. All lines masked until a driver asks for one. */
#ifndef MIRIX_DRIVERS_PIC_H
#define MIRIX_DRIVERS_PIC_H

#include <stdint.h>

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xa0
#define PIC2_DATA 0xa1

#define IRQ_BASE 0x20

void pic_init(void);
void pic_set_mask(uint8_t irq);    /* irq 0-15 */
void pic_clear_mask(uint8_t irq);
void pic_eoi(uint8_t vector);      /* vector = IRQ_BASE + irq */

#endif
