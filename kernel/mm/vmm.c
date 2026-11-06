#include "vmm.h"
#include "mm.h"
#include "pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"
#include "lib/panic.h"

/* PML4 of the current address space, accessed via the HHDM. */
static uint64_t *pml4(void)
{
    return (uint64_t *)phys_to_virt(read_cr3() & PTE_ADDR_MASK);
}

void vmm_invlpg(uint64_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

/* Return a pointer (via HHDM) to the child table of `entry`, allocating and
 * zeroing a new table from the PMM if the entry is not present. `slot` is
 * where the entry lives. */
static uint64_t *walk_alloc(uint64_t *slot, uint64_t flags)
{
    uint64_t entry = *slot;
    if (!(entry & PTE_PRESENT)) {
        uint64_t np = pmm_alloc_page();
        if (np == 0)
            kpanic("vmm: out of physical pages for a page table\n");
        memset(phys_to_virt(np), 0, PAGE_SIZE);
        uint64_t f = PTE_PRESENT | PTE_RW;
        if (flags & PTE_USER)
            f |= PTE_USER;
        *slot = np | f;
        entry = *slot;
    } else if ((flags & PTE_USER) && !(entry & PTE_USER)) {
        /* Existing table (e.g. Limine's low-half mapping) without USER:
         * user access is denied unless EVERY level has U=1. */
        *slot = entry | PTE_USER;
    }
    return (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
}

static uint64_t idx(uint64_t v, int level)   /* level 4=PML4 .. 1=PT */
{
    return (v >> (12 + 9 * (level - 1))) & 0x1ff;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4e = &pml4()[idx(virt, 4)];
    uint64_t *pdpt = walk_alloc(pml4e, flags);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    uint64_t *pd = walk_alloc(pde, flags);
    uint64_t *pte = &pd[idx(virt, 2)];
    uint64_t *pt = walk_alloc(pte, flags);
    pt[idx(virt, 1)] = (phys & PTE_ADDR_MASK) | flags;
    vmm_invlpg(virt);
}

void vmm_unmap(uint64_t virt)
{
    uint64_t *pml4e = &pml4()[idx(virt, 4)];
    if (!(*pml4e & PTE_PRESENT)) return;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(*pml4e & PTE_ADDR_MASK);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    if (!(*pde & PTE_PRESENT)) return;
    uint64_t *pd = (uint64_t *)phys_to_virt(*pde & PTE_ADDR_MASK);
    uint64_t *pte = &pd[idx(virt, 2)];
    if (!(*pte & PTE_PRESENT)) return;
    uint64_t *pt = (uint64_t *)phys_to_virt(*pte & PTE_ADDR_MASK);
    pt[idx(virt, 1)] = 0;
    vmm_invlpg(virt);
}

uint64_t vmm_create_space(void)
{
    uint64_t pa = pmm_alloc_page();
    if (pa == 0)
        kpanic("vmm: OOM creating address space\n");
    uint64_t *dst = (uint64_t *)phys_to_virt(pa);
    uint64_t *src = pml4();
    memset(dst, 0, PAGE_SIZE);
    for (int i = 256; i < 512; i++)   /* kernel half shared */
        dst[i] = src[i];
    return pa;
}

uint64_t vmm_get_physical(uint64_t virt)
{
    uint64_t *pml4e = &pml4()[idx(virt, 4)];
    if (!(*pml4e & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(*pml4e & PTE_ADDR_MASK);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    if (!(*pde & PTE_PRESENT)) return 0;
    uint64_t *pd = (uint64_t *)phys_to_virt(*pde & PTE_ADDR_MASK);
    uint64_t *pte = &pd[idx(virt, 2)];
    if (!(*pte & PTE_PRESENT)) return 0;
    /* handle 2 MiB huge pages just in case */
    if (*pte & (1ULL << 7))
        return (*pte & 0x000fffffffe00000ULL) | (virt & 0x1fffff);
    uint64_t *pt = (uint64_t *)phys_to_virt(*pte & PTE_ADDR_MASK);
    uint64_t e = pt[idx(virt, 1)];
    if (!(e & PTE_PRESENT)) return 0;
    return (e & PTE_ADDR_MASK) | (virt & 0xfff);
}
