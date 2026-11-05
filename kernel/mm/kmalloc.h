/* Kernel heap: first-fit free-list allocator over a virtual range in the
 * higher half (KHEAP_VIRT), grown on demand by mapping PMM pages. */
#ifndef MIRIX_MM_KMALLOC_H
#define MIRIX_MM_KMALLOC_H

#include <stddef.h>
#include <stdint.h>

void heap_init(void);
void *kmalloc(size_t size);
void kfree(void *p);
void heap_stats(uint64_t *used_bytes, uint64_t *free_bytes, uint64_t *nblocks);
int heap_selftest(void);   /* 1 = pass */

#endif
