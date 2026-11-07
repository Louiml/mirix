#include "ioapic.h"
#include "acpi.h"
#include "drivers/pic.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "mm/vmm.h"

/* One 4 KiB MMIO page. This VA deliberately shares the PD entry of the
 * proven AHCI mapping (AHCI_VIRT = 0xffffffffe1000000, same 2 MiB region):
 * 0xffffffffe2000000 sits under a Limine 1 GiB huge-page entry, so vmm_map
 * descends into the huge page and the PTE never lands (#PF on first access). */
/* Use the same 2 MiB region as the vmm self-test (0xffffffffd0000000 passed
 * map/unmap + data roundtrip), not the e1xxxxxx area where Limine's page
 * tables have huge-page entries that vmm_map cannot split. */
#define IOAPIC_VIRT 0xffffffffd0001000ULL

static volatile uint32_t *g_reg;    /* [0] = register select, [4] = data (0x10) */
static int g_active;

static uint32_t ioapic_read(uint32_t reg)
{
    g_reg[0] = reg;
    return g_reg[4];
}

static void ioapic_write(uint32_t reg, uint32_t val)
{
    g_reg[0] = reg;
    g_reg[4] = val;
}

/* Redirection entry low dword: vector | edge(0) | active-high | mask bit. */
static void ioapic_redirect(uint32_t pin, uint8_t vector, int unmasked)
{
    uint32_t lo = vector;
    if (!unmasked)
        lo |= 0x10000U;
    ioapic_write(0x10 + pin * 2, lo);
    ioapic_write(0x11 + pin * 2, 0);    /* destination APIC ID 0 = BSP */
}

/* ISA devices (i8042) hold their IRQ line asserted until the byte is read,
 * so route pins 1/12 level-triggered, active-high; EOI goes to the IO-APIC
 * EOI register (0x40), which also broadcasts to the local APIC. */
static void ioapic_redirect_level(uint32_t pin, uint8_t vector)
{
    uint32_t lo = vector | (1U << 15);    /* trigger mode = level */
    ioapic_write(0x10 + pin * 2, lo);
    ioapic_write(0x11 + pin * 2, 0);      /* destination APIC ID 0 = BSP */
}

int ioapic_setup(int force_pic)
{
    if (force_pic) {
        uart_printf("ioapic: skipped (cmdline force_pic) -> legacy PIC mode\n");
        return 0;
    }
    uint32_t mlen = 0;
    const uint8_t *madt = (const uint8_t *)acpi_find_table("APIC", &mlen);
    if (!madt || mlen < 44) {
        uart_printf("ioapic: no MADT (APIC) table -> legacy PIC mode\n");
        return 0;
    }
    /* MADT: SDT header (36) + local APIC addr (4) + flags (4), then interrupt
     * controller structures { u8 type, u8 len, ... }. Type 1 = I/O APIC:
     * { u8 id, u8 reserved, u32 gsi_base, u64 ioapic_phys } (len 12). */
    uint64_t phys = 0;
    uint32_t gsi_base = 0;
    int found = 0;
    uint32_t off = 44;
    while (off + 2 <= mlen) {
        uint8_t type = madt[off];
        uint8_t len = madt[off + 1];
        if (len < 2 || off + (uint32_t)len > mlen)
            break;
        if (type == 1 && len >= 12) {
            gsi_base = *(const uint32_t *)(madt + off + 4);
            phys = *(const uint64_t *)(madt + off + 8);
            found = 1;
            break;
        }
        off += len;
    }
    if (!found) {
        uart_printf("ioapic: MADT has no I/O APIC entry -> legacy PIC mode\n");
        return 0;
    }
    /* Sanity: a real I/O APIC MMIO address lives in the low 4 GiB PCI hole.
     * If the MADT parse produced garbage, fall back to the architectural
     * default (QEMU q35 and VirtualBox both use 0xfec00000). */
    if (phys < 0x40000000ULL || phys >= 0x100000000ULL) {
        uart_printf("ioapic: MADT phys %p out of range, using default 0xfec00000\n",
                    (void *)phys);
        phys = 0xfec00000ULL;
        gsi_base = 0;
    }
    uint64_t page = phys & ~0xfULL;
    vmm_map(IOAPIC_VIRT, page, PTE_PRESENT | PTE_RW);
    if (vmm_get_physical(IOAPIC_VIRT) != page) {
        /* Defensive: never touch an MMIO VA whose PTE did not land (that is
         * what caused a #PF here before). Fall back to the legacy PIC. */
        uart_printf("ioapic: map verify failed for %p -> legacy PIC mode\n",
                    (void *)IOAPIC_VIRT);
        return 0;
    }
    g_reg = (volatile uint32_t *)IOAPIC_VIRT;
    uint32_t ver = ioapic_read(1);
    uint32_t pins = ((ver >> 16) & 0xff) + 1;
    for (uint32_t p = 0; p < pins; p++)
        ioapic_redirect(p, 0, 0);           /* mask everything first */
    ioapic_redirect(1, IRQ_BASE + 1, 1);     /* PS/2 keyboard -> vector 33 */
    ioapic_redirect(12, IRQ_BASE + 12, 1);   /* PS/2 mouse    -> vector 44 */
    g_active = 1;
    uart_printf("ioapic: phys %p gsi_base %u mapped at %p, %u pins; "
                "pin1->vec %d, pin12->vec %d (8259 stays masked)\n",
                (void *)phys, gsi_base, (void *)IOAPIC_VIRT, pins,
                IRQ_BASE + 1, IRQ_BASE + 12);
    return 1;
}

int ioapic_active(void) { return g_active; }

/* EOI for an IO-APIC-delivered interrupt: writing the EOI register broadcasts
 * end-of-interrupt to the local APIC(s), so no separate LAPIC EOI is needed. */
void ioapic_eoi(uint8_t vector) { ioapic_write(0x40, vector); }
