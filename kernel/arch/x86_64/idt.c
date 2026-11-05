#include "idt.h"
#include "gdt.h"
#include "cpu.h"
#include "lib/uart.h"
#include "lib/panic.h"
#include "drivers/pic.h"
#include "drivers/timer.h"
#include "drivers/lapic.h"

/* Stub address tables built in isr.asm */
extern const uint64_t isr_stub_table[32];
extern const uint64_t irq_stub_table[32];

static idt_entry_t g_idt[256];
static idtr_t g_idtr;

static const char *exception_names[32] = {
    [0] = "#DE divide error",      [1] = "#DB debug",              [2] = "NMI",
    [3] = "#BP breakpoint",        [4] = "#OF overflow",           [5] = "#BR bound range",
    [6] = "#UD invalid opcode",    [7] = "#NM device not available",[8] = "#DF double fault",
    [9] = "coprocessor segment overrun", [10] = "#TS invalid TSS",  [11] = "#NP segment not present",
    [12] = "#SS stack fault",      [13] = "#GP general protection", [14] = "#PF page fault",
    [15] = "reserved",             [16] = "#MF x87 error",         [17] = "#AC alignment check",
    [18] = "#MC machine check",    [19] = "#XM SIMD error",        [20] = "#VE virtualization",
    [21] = "#CP control protection", [22] = "reserved",            [23] = "reserved",
    [24] = "reserved",             [25] = "reserved",              [26] = "reserved",
    [27] = "reserved",             [28] = "#HV hypervisor injection", [29] = "#VC VMM communication",
    [30] = "#SX security",         [31] = "reserved",
};

static void idt_set(int vector, uint64_t handler, uint8_t ist)
{
    idt_entry_t *e = &g_idt[vector];
    e->offset_low = handler & 0xffff;
    e->selector = SEL_KCODE;
    e->ist = ist;
    e->flags = 0x8e;                 /* present, ring 0, interrupt gate */
    e->offset_mid = (handler >> 16) & 0xffff;
    e->offset_high = (handler >> 32) & 0xffffffff;
    e->reserved = 0;
}

void idt_init(void)
{
    g_idtr.limit = sizeof(g_idt) - 1;
    g_idtr.base = (uint64_t)&g_idt;

    for (int i = 0; i < 32; i++)
        idt_set(i, isr_stub_table[i], i == 8 ? 1 : 0);  /* IST1 for #DF */
    for (int i = 0; i < 32; i++)
        idt_set(32 + i, irq_stub_table[i], 0);
    /* 64..255 left null: any such interrupt triple-faults (good: loud bug) */

    __asm__ volatile ("lidt %0" : : "m"(g_idtr));
    uart_printf("idt: 256 gates loaded, exceptions 0-31 + IRQs 32-63\n");
}

void isr_handler(registers_t *regs)
{
    if (regs->vector < 32) {
        uart_printf("exception: %s (vector %llu, error 0x%llx) at rip=%p\n",
                    exception_names[regs->vector],
                    (unsigned long long)regs->vector,
                    (unsigned long long)regs->error, (void *)regs->rip);
        panic_dump_regs(regs);   /* prints full dump + red screen; no return */
    }
    kpanic("unexpected isr vector %llu\n", (unsigned long long)regs->vector);
}

void irq_handler(registers_t *regs)
{
    uint64_t v = regs->vector;
    if (v == TIMER_IRQ_VECTOR) {
        timer_irq();          /* ticks++ + LAPIC EOI (drivers/timer.c) */
        return;
    }
    if (v >= 32 + 16) {
        /* Other local APIC LVT interrupt: EOI to the LAPIC */
        lapic_eoi();
    } else {
        pic_eoi((uint8_t)v);
    }
    /* No other IRQ sources are unmasked yet; this only fires on spurious. */
    uart_printf("irq %llu (spurious?)\n", (unsigned long long)(v - 32));
}
