#include "kmalloc.h"
#include "mm.h"
#include "pmm.h"
#include "vmm.h"
#include "lib/string.h"
#include "lib/uart.h"
#include "lib/spinlock.h"

/* SMP (8c): the block list is shared by all CPUs — every mutator takes this.
 * Lock order: callers may hold sched/vfs/wm, then heap, then pmm. */
static spinlock_t g_heap_lock = SPINLOCK_INIT;

#define KHEAP_VIRT      0xffffffffc0000000ULL
#define KHEAP_MAX_GROW  (32ULL << 20)     /* 32 MiB ceiling for now */
#define ALIGN16(x)      (((x) + 15) & ~15ULL)

typedef struct block {
    size_t size;            /* payload bytes (excludes this header) */
    int free;
    struct block *next;     /* address-ordered list */
} block_t;

static block_t *g_blocks;       /* head of address-ordered block list */
static uint64_t g_heap_top;     /* next unmapped virtual address */
static uint64_t g_heap_grown;   /* total bytes mapped so far */

void heap_init(void)
{
    g_blocks = 0;
    g_heap_top = KHEAP_VIRT;
    g_heap_grown = 0;
}

/* Map `bytes` more heap pages; returns the old top (base of the new chunk). */
static uint64_t heap_grow(uint64_t bytes)
{
    if (g_heap_grown + bytes > KHEAP_MAX_GROW)
        return 0;
    uint64_t base = g_heap_top;
    uint64_t pages = PAGE_ALIGN_UP(bytes) / PAGE_SIZE;
    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = pmm_alloc_page();
        if (phys == 0)
            return 0;
        memset(phys_to_virt(phys), 0, PAGE_SIZE);
        vmm_map(g_heap_top + i * PAGE_SIZE, phys, PTE_PRESENT | PTE_RW);
    }
    g_heap_top += pages * PAGE_SIZE;
    g_heap_grown += pages * PAGE_SIZE;
    return base;
}

static void append_block(block_t *b)
{
    b->next = 0;
    if (!g_blocks) {
        g_blocks = b;
        return;
    }
    block_t *t = g_blocks;
    while (t->next)
        t = t->next;
    t->next = b;
    /* if the previous block is free and adjacent, merge into it */
    if (t->free && (char *)t + sizeof(block_t) + t->size == (char *)b) {
        t->size += sizeof(block_t) + b->size;
        t->next = b->next;
    }
}

static void *kmalloc_locked(size_t size)
{
    if (size == 0)
        size = 1;
    size = ALIGN16(size);

    /* first fit */
    for (block_t *b = g_blocks; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;
        /* split if the remainder can hold a header + 16 bytes */
        if (b->size >= size + sizeof(block_t) + 16) {
            block_t *nb = (block_t *)((char *)b + sizeof(block_t) + size);
            nb->size = b->size - size - sizeof(block_t);
            nb->free = 1;
            nb->next = b->next;
            b->next = nb;
            b->size = size;
        }
        b->free = 0;
        return (char *)b + sizeof(block_t);
    }

    /* no fit: grow the heap and add a block there */
    uint64_t need = sizeof(block_t) + size;
    uint64_t base = heap_grow(need);
    if (base == 0)
        return 0;
    block_t *b = (block_t *)base;
    b->size = size;
    b->free = 0;
    append_block(b);
    return (char *)b + sizeof(block_t);
}

static void kfree_locked(void *p)
{
    if (!p)
        return;
    block_t *b = (block_t *)((char *)p - sizeof(block_t));
    b->free = 1;
    /* coalesce forward */
    while (b->free && b->next &&
           (char *)b + sizeof(block_t) + b->size == (char *)b->next) {
        b->size += sizeof(block_t) + b->next->size;
        b->next = b->next->next;
    }
}

void *kmalloc(size_t size)
{
    uint64_t fl = spin_lock_irqsave(&g_heap_lock);
    void *r = kmalloc_locked(size);
    spin_unlock_irqrestore(&g_heap_lock, fl);
    return r;
}

void kfree(void *p)
{
    uint64_t fl = spin_lock_irqsave(&g_heap_lock);
    kfree_locked(p);
    spin_unlock_irqrestore(&g_heap_lock, fl);
}

void heap_stats(uint64_t *used, uint64_t *freeb, uint64_t *nblocks)
{
    uint64_t fl = spin_lock_irqsave(&g_heap_lock);
    uint64_t u = 0, f = 0, n = 0;
    for (block_t *b = g_blocks; b; b = b->next) {
        n++;
        if (b->free) f += b->size; else u += b->size;
    }
    spin_unlock_irqrestore(&g_heap_lock, fl);
    *used = u; *freeb = f; *nblocks = n;
}

int heap_selftest(void)
{
    enum { N = 64 };
    static void *ptrs[N];
    static size_t sizes[N];

    /* deterministic pseudo-random sizes */
    uint32_t seed = 0x12345678;
    for (int i = 0; i < N; i++) {
        seed = seed * 1664525 + 1013904223;
        sizes[i] = (seed >> 16) % 512 + 16;
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i])
            return 0;
        memset(ptrs[i], 0xA0 + i, sizes[i]);
    }
    /* verify all */
    for (int i = 0; i < N; i++)
        for (size_t j = 0; j < sizes[i]; j++)
            if (((uint8_t *)ptrs[i])[j] != (uint8_t)(0xA0 + i))
                return 0;
    /* free every other block, then allocate again */
    for (int i = 0; i < N; i += 2)
        kfree(ptrs[i]);
    for (int i = 0; i < N; i += 2) {
        ptrs[i] = kmalloc(sizes[i]);
        if (!ptrs[i])
            return 0;
        memset(ptrs[i], 0x5A, sizes[i]);
    }
    /* the untouched odd blocks must be intact */
    for (int i = 1; i < N; i += 2)
        for (size_t j = 0; j < sizes[i]; j++)
            if (((uint8_t *)ptrs[i])[j] != (uint8_t)(0xA0 + i))
                return 0;
    /* the reallocated ones must hold the new pattern */
    for (int i = 0; i < N; i += 2)
        for (size_t j = 0; j < sizes[i]; j++)
            if (((uint8_t *)ptrs[i])[j] != 0x5A)
                return 0;
    for (int i = 0; i < N; i++)
        kfree(ptrs[i]);

    uint64_t u, f, n;
    heap_stats(&u, &f, &n);
    uart_printf("heap self-test: PASS (%llu blocks, %llu B used, %llu B free after freeing all)\n",
                (unsigned long long)n, (unsigned long long)u, (unsigned long long)f);
    return 1;
}
