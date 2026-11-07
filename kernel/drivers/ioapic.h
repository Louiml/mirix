/* IO-APIC support (Phase 9b).
 *
 * On machines whose ACPI MADT advertises an I/O APIC — QEMU q35, and
 * VirtualBox whenever "Enable I/O APIC" is on (the default for multi-vCPU
 * VMs) — the ISA IRQs we care about (keyboard IRQ1, PS/2 mouse IRQ12) are
 * delivered through the IO-APIC, not the legacy 8259 PIC, so a PIC-only
 * driver never sees them in those VMs. We map the IO-APIC MMIO, mask all
 * pins, then redirect pins 1 and 12 to our usual IDT vectors (IRQ_BASE+1 /
 * IRQ_BASE+12), edge-triggered, destination = BSP APIC ID. In IO-APIC mode
 * the 8259 stays fully masked. Without a MADT I/O APIC entry (or with the
 * cmdline flag "force_pic") the legacy PIC path is used unchanged. */
#ifndef MIRIX_DRIVERS_IOAPIC_H
#define MIRIX_DRIVERS_IOAPIC_H

#include <stdint.h>

int  ioapic_setup(int force_pic);   /* 1 if the IO-APIC now routes IRQ1/12 */
int  ioapic_active(void);
void ioapic_eoi(uint8_t vector);    /* EOI register write (broadcasts to LAPICs) */

#endif
