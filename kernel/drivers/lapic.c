#include "lapic.h"
#include "mm/mm.h"
#include "arch/x86_64/cpu.h"
#include "lib/uart.h"

#define MSR_IA32_APIC_BASE 0x1b
#define APIC_BASE_GLOBAL_ENABLE (1ULL << 11)
#define APIC_BASE_XAPIC_MODE    (1ULL << 10)

static volatile uint32_t *regs(void)
{
    return (volatile uint32_t *)phys_to_virt(LAPIC_PHYS_BASE);
}

uint32_t lapic_read(uint32_t offset)
{
    return regs()[offset / 4];
}

void lapic_write(uint32_t offset, uint32_t value)
{
    regs()[offset / 4] = value;
}

void lapic_init(void)
{
    /* Make sure the local APIC is globally enabled and in xAPIC mode. */
    uint64_t base = rdmsr(MSR_IA32_APIC_BASE);
    base |= APIC_BASE_GLOBAL_ENABLE | APIC_BASE_XAPIC_MODE;
    base &= ~0xfffffULL;                 /* keep reserved bits, force phys base */
    base |= LAPIC_PHYS_BASE;
    wrmsr(MSR_IA32_APIC_BASE, base);

    /* Spurious vector: 0xff, software enabled. */
    lapic_write(LAPIC_SVR, 0xff | LAPIC_SVR_ENABLE);
    uart_printf("lapic: enabled (xAPIC, base phys %p, SVR=0x%x)\n",
                (void *)LAPIC_PHYS_BASE, lapic_read(LAPIC_SVR));
}

void lapic_timer_oneshot(uint8_t vector, uint32_t count)
{
    lapic_write(LAPIC_TIMER_DIV, 0xb);                 /* divide by 1 */
    lapic_write(LAPIC_TIMER_LVT, vector);              /* one-shot, unmasked */
    lapic_write(LAPIC_TIMER_INIT, count);
}

void lapic_timer_periodic(uint8_t vector, uint32_t count)
{
    lapic_write(LAPIC_TIMER_DIV, 0xb);                 /* divide by 1 */
    lapic_write(LAPIC_TIMER_LVT, vector | (1u << 17)); /* periodic, unmasked */
    lapic_write(LAPIC_TIMER_INIT, count);
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}
