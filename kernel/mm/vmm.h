/* Virtual memory: helpers to map/unmap pages in the current address space
 * (Limine's PML4). New page-table levels are allocated from the PMM and
 * accessed through the HHDM. */
#ifndef MIRIX_MM_VMM_H
#define MIRIX_MM_VMM_H

#include <stdint.h>

#define PTE_PRESENT (1ULL << 0)
#define PTE_RW      (1ULL << 1)
#define PTE_USER    (1ULL << 2)
#define PTE_NX      (1ULL << 63)

#define PTE_ADDR_MASK 0x000ffffffffff000ULL

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
uint64_t vmm_get_physical(uint64_t virt);   /* 0 if unmapped */
void vmm_invlpg(uint64_t virt);

/* Create a new address space: fresh PML4 with the kernel half (entries
 * 256-511: HHDM, kernel image, heap) copied from the current one.
 * Returns the physical address of the new PML4. */
uint64_t vmm_create_space(void);

/* Map phys into ANOTHER address space (given its PML4 phys) — used to give a
 * user process a shared window buffer. Walks the tables via the HHDM, so no
 * CR3 switch is needed. */
void vmm_map_in_space(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                      uint64_t flags);

#endif
