/* Shared memory constants/helpers. The HHDM (higher-half direct map) lets us
 * touch any physical address as hhdm_offset + phys — Limine set it up. */
#ifndef MIRIX_MM_MM_H
#define MIRIX_MM_MM_H

#include <stdint.h>

#define PAGE_SIZE 4096ULL
#define PAGE_ALIGN_UP(x)   (((x) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_ALIGN_DOWN(x) ((x) & ~(PAGE_SIZE - 1))

extern uint64_t g_hhdm_offset;   /* defined in main.c */

static inline void *phys_to_virt(uint64_t p) { return (void *)(g_hhdm_offset + p); }
static inline uint64_t virt_to_phys_hhdm(uint64_t v) { return v - g_hhdm_offset; }

#endif
