#include "pmm.h"
#include "mm.h"
#include "lib/string.h"
#include "lib/uart.h"
#include "lib/panic.h"

static uint8_t *g_bitmap;        /* accessed via HHDM */
static uint64_t g_bitmap_phys;
static uint64_t g_total_pages;   /* bits in bitmap */
static uint64_t g_free_pages;

static inline void bm_set(uint64_t i)   { g_bitmap[i >> 3] |=  (1 << (i & 7)); }
static inline void bm_clear(uint64_t i) { g_bitmap[i >> 3] &= ~(1 << (i & 7)); }
static inline int  bm_test(uint64_t i)  { return (g_bitmap[i >> 3] >> (i & 7)) & 1; }

void pmm_init(struct limine_memmap_response *mm)
{
    /* Size the bitmap to the highest USABLE address (reserved MMIO holes
     * above RAM don't need bits — we never allocate from them). */
    uint64_t max_addr = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->base + e->length > max_addr)
            max_addr = e->base + e->length;
    }
    if (max_addr == 0)
        kpanic("pmm: no usable memory in memmap\n");

    g_total_pages = PAGE_ALIGN_UP(max_addr) / PAGE_SIZE;
    uint64_t bitmap_bytes = PAGE_ALIGN_UP((g_total_pages + 7) / 8);

    /* Place the bitmap at the start of the first usable region that fits. */
    uint64_t base = 0;
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_bytes + PAGE_SIZE) {
            base = e->base;
            break;
        }
    }
    if (base == 0)
        kpanic("pmm: no usable region big enough for the bitmap (%llu bytes)\n",
               (unsigned long long)bitmap_bytes);

    g_bitmap_phys = base;
    g_bitmap = phys_to_virt(base);

    /* Start with everything used, then free the usable regions. */
    memset(g_bitmap, 0xff, bitmap_bytes);
    for (uint64_t i = 0; i < mm->entry_count; i++) {
        struct limine_memmap_entry *e = mm->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        uint64_t first = PAGE_ALIGN_UP(e->base) / PAGE_SIZE;
        uint64_t last = (e->base + e->length) / PAGE_SIZE;   /* exclusive */
        if (last > g_total_pages)
            last = g_total_pages;
        for (uint64_t p = first; p < last; p++)
            bm_clear(p);
    }
    /* The bitmap itself, and page 0 (NULL page must never be handed out). */
    for (uint64_t p = base / PAGE_SIZE; p < (base + bitmap_bytes) / PAGE_SIZE; p++)
        bm_set(p);
    bm_set(0);

    g_free_pages = 0;
    for (uint64_t p = 0; p < g_total_pages; p++)
        if (!bm_test(p))
            g_free_pages++;

    uart_printf("pmm: bitmap at phys %p (%llu bytes), covers %llu pages (%llu MiB), free %llu pages (%llu MiB)\n",
                (void *)g_bitmap_phys, (unsigned long long)bitmap_bytes,
                (unsigned long long)g_total_pages,
                (unsigned long long)(g_total_pages * PAGE_SIZE >> 20),
                (unsigned long long)g_free_pages,
                (unsigned long long)(g_free_pages * PAGE_SIZE >> 20));
}

uint64_t pmm_alloc_page(void)
{
    /* Word-skip scan: a full 0xff qword has no free page. */
    uint64_t words = (g_total_pages + 63) / 64;
    uint64_t *bm = (uint64_t *)g_bitmap;
    for (uint64_t w = 0; w < words; w++) {
        if (bm[w] == 0xffffffffffffffffULL)
            continue;
        for (int b = 0; b < 64; b++) {
            uint64_t i = w * 64 + b;
            if (i >= g_total_pages)
                break;
            if (!bm_test(i)) {
                bm_set(i);
                g_free_pages--;
                return i * PAGE_SIZE;
            }
        }
    }
    return 0;
}

void pmm_free_page(uint64_t phys)
{
    uint64_t i = phys / PAGE_SIZE;
    if (i >= g_total_pages || !bm_test(i))
        return;   /* double free / out of range: ignore quietly */
    bm_clear(i);
    g_free_pages++;
}

uint64_t pmm_free_pages(void) { return g_free_pages; }
uint64_t pmm_total_pages(void) { return g_total_pages; }
