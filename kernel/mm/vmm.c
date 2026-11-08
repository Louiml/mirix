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
 * where the entry lives; `level` is 4=PML4, 3=PDPT, 2=PD (used to split huge
 * pages). If the entry is a huge page (1 GiB at PDPT, 2 MiB at PD) we split it
 * into a full child table of smaller mappings — otherwise descending into the
 * huge page as if it were a table silently loses the new PTE and the first
 * access #PFs (this is what broke the first IO-APIC mapping, and can also hit
 * heap growth on VMs whose Limine tables use huge pages for whole regions). */
static uint64_t *walk_alloc(uint64_t *slot, int level, uint64_t flags)
{
    uint64_t entry = *slot;
    uint64_t f = PTE_PRESENT | PTE_RW;
    if (flags & PTE_USER)
        f |= PTE_USER;
    if (!(entry & PTE_PRESENT)) {
        uint64_t np = pmm_alloc_page();
        if (np == 0)
            kpanic("vmm: out of physical pages for a page table\n");
        memset(phys_to_virt(np), 0, PAGE_SIZE);
        *slot = np | f;
        return (uint64_t *)phys_to_virt(np);
    }
    if (entry & (1ULL << 7)) {          /* PS = huge page at PDPT/PD */
        uint64_t np = pmm_alloc_page();
        if (np == 0)
            kpanic("vmm: out of physical pages splitting a huge page\n");
        uint64_t *t = (uint64_t *)phys_to_virt(np);
        memset(t, 0, PAGE_SIZE);
        uint64_t keep = entry & ~PTE_ADDR_MASK;   /* keep flags (incl. NX) */
        if (level == 3) {               /* 1 GiB page -> 512 x 2 MiB pages */
            uint64_t base = entry & ~((1ULL << 30) - 1);
            for (int i = 0; i < 512; i++)
                t[i] = (base + (uint64_t)i * (1ULL << 21))
                       | (keep & ~(1ULL << 7)) | (1ULL << 7);
        } else {                        /* 2 MiB page -> 512 x 4 KiB pages */
            uint64_t base = entry & ~((1ULL << 21) - 1);
            for (int i = 0; i < 512; i++)
                t[i] = (base + (uint64_t)i * PAGE_SIZE) | (keep & ~(1ULL << 7));
        }
        *slot = np | f;
        return t;
    }
    if ((flags & PTE_USER) && !(entry & PTE_USER))
        *slot = entry | PTE_USER;
    return (uint64_t *)phys_to_virt(entry & PTE_ADDR_MASK);
}

static uint64_t idx(uint64_t v, int level)   /* level 4=PML4 .. 1=PT */
{
    return (v >> (12 + 9 * (level - 1))) & 0x1ff;
}

void vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t *pml4e = &pml4()[idx(virt, 4)];
    uint64_t *pdpt = walk_alloc(pml4e, 4, flags);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    uint64_t *pd = walk_alloc(pde, 3, flags);
    uint64_t *pte = &pd[idx(virt, 2)];
    uint64_t *pt = walk_alloc(pte, 2, flags);
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

void vmm_map_in_space(uint64_t pml4_phys, uint64_t virt, uint64_t phys,
                      uint64_t flags)
{
    uint64_t *root = (uint64_t *)phys_to_virt(pml4_phys);
    uint64_t *pml4e = &root[idx(virt, 4)];
    uint64_t *pdpt = walk_alloc(pml4e, 4, flags);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    uint64_t *pd = walk_alloc(pde, 3, flags);
    uint64_t *pte = &pd[idx(virt, 2)];
    uint64_t *pt = walk_alloc(pte, 2, flags);
    pt[idx(virt, 1)] = (phys & PTE_ADDR_MASK) | flags;
    /* No invlpg: the target space isn't loaded right now (or, if it is the
     * current one, the entry was just created so the TLB can't hold a stale
     * mapping for it — and walk_alloc only adds USER to existing entries). */
}

uint64_t vmm_get_physical(uint64_t virt)
{
    uint64_t *pml4e = &pml4()[idx(virt, 4)];
    if (!(*pml4e & PTE_PRESENT)) return 0;
    uint64_t *pdpt = (uint64_t *)phys_to_virt(*pml4e & PTE_ADDR_MASK);
    uint64_t *pde = &pdpt[idx(virt, 3)];
    if (!(*pde & PTE_PRESENT)) return 0;
    if (*pde & (1ULL << 7))   /* 1 GiB huge page at PDPT */
        return (*pde & ~((1ULL << 30) - 1)) | (virt & ((1ULL << 30) - 1));
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
