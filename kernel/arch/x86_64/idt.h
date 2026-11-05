/* IDT: 256 interrupt gates. Entry 0-31 = CPU exceptions, 32+ = IRQs
 * (after PIC remap to 0x20+). */
#ifndef MIRIX_ARCH_IDT_H
#define MIRIX_ARCH_IDT_H

#include <stdint.h>

typedef struct {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  flags;        /* 0x8E = present, ring 0, interrupt gate */
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed)) idt_entry_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) idtr_t;

/* Register frame pushed by the ISR stubs (see isr.asm). Order matches the
 * pushes: rax was pushed last, so it is at the lowest address. */
typedef struct {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t vector;
    uint64_t error;
    uint64_t rip, cs, rflags, rsp, ss;
} registers_t;

void idt_init(void);

/* C handlers called from isr.asm */
void isr_handler(registers_t *regs);
void irq_handler(registers_t *regs);

#endif
