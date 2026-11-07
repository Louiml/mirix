/* ACPI (Phase 8b): RSDP (from the Limine RSDP request) -> XSDT -> FADT,
 * DSDT scan for the _S5 package (SLP_TYP), then S5 poweroff by writing
 * SLP_TYP|SLP_EN to the PM1a_CNT I/O port. On QEMU (-no-reboot) this makes
 * the process exit cleanly. */
#ifndef MIRIX_DRIVERS_ACPI_H
#define MIRIX_DRIVERS_ACPI_H

#include <stdint.h>

/* rsdp_phys: physical address of the RSDP (Limine rsdp_request response).
 * Returns 0 on success. */
int  acpi_init(uint64_t rsdp_phys);

/* Write SLP_EN|SLP_TYP(S5) to PM1a_CNT. Prints "acpi: poweroff" first.
 * Does not return on real hardware/QEMU (S5); caller should halt after. */
void acpi_poweroff(void);
/* Phase 9b: find an ACPI table by 4-char signature (e.g. "APIC" = MADT).
 * Returns the table (HHDM-mapped) or 0; optional length out-param. */
const void *acpi_find_table(const char *sig, uint32_t *len_out);

#endif
