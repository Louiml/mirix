/* Physical memory manager: one bitmap bit per 4 KiB page.
 * 1 = used, 0 = free. Covers all `usable` RAM from the Limine memmap. */
#ifndef MIRIX_MM_PMM_H
#define MIRIX_MM_PMM_H

#include <stdint.h>
#include "limine.h"

void pmm_init(struct limine_memmap_response *mm);
uint64_t pmm_alloc_page(void);      /* physical address, 0 on OOM */
void pmm_free_page(uint64_t phys);
uint64_t pmm_free_pages(void);
uint64_t pmm_total_pages(void);

#endif
