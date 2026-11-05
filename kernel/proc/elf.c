#include "elf.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/string.h"
#include "lib/uart.h"

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf64_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} elf64_phdr_t;

#define PT_LOAD 1
#define ET_EXEC 2
#define EM_X86_64 62

/* Ensure `va` is mapped with USER. Limine identity-maps the low half
 * (phys == va, no USER bit) — replace those with fresh zeroed pages.
 * Pages we already mapped during this load (phys != va) are kept, so
 * overlapping PT_LOAD segments don't clobber each other. */
static int ensure_user_page(uint64_t va)
{
    uint64_t cur = vmm_get_physical(va);
    if (cur == va) {            /* Limine identity mapping */
        vmm_unmap(va);
        cur = 0;
    }
    if (cur == 0) {
        uint64_t pa = pmm_alloc_page();
        if (pa == 0)
            return 0;
        memset(phys_to_virt(pa), 0, PAGE_SIZE);
        vmm_map(va, pa, PTE_PRESENT | PTE_RW | PTE_USER);
    }
    return 1;
}

static int map_user_range(uint64_t vaddr, uint64_t bytes)
{
    uint64_t start = PAGE_ALIGN_DOWN(vaddr);
    uint64_t end = PAGE_ALIGN_UP(vaddr + bytes);
    for (uint64_t va = start; va < end; va += PAGE_SIZE)
        if (!ensure_user_page(va))
            return 0;
    return 1;
}

uint64_t elf_load(const uint8_t *blob, size_t len)
{
    if (len < sizeof(elf64_ehdr_t))
        return 0;
    const elf64_ehdr_t *eh = (const elf64_ehdr_t *)blob;
    if (eh->e_ident[0] != 0x7f || eh->e_ident[1] != 'E' ||
        eh->e_ident[2] != 'L' || eh->e_ident[3] != 'F')
        return 0;
    if (eh->e_type != ET_EXEC || eh->e_machine != EM_X86_64)
        return 0;

    uart_printf("elf: loading %llu-byte executable, entry=%p, %u phdrs\n",
                (unsigned long long)len, (void *)eh->e_entry, eh->e_phnum);

    /* Pass 1: map every segment's pages (and the stack) exactly once. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(blob + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        uart_printf("elf: PT_LOAD vaddr=%p filesz=%llu memsz=%llu off=%llu\n",
                    (void *)ph->p_vaddr, (unsigned long long)ph->p_filesz,
                    (unsigned long long)ph->p_memsz, (unsigned long long)ph->p_offset);
        if (!map_user_range(ph->p_vaddr, ph->p_memsz)) {
            uart_printf("elf: OOM mapping segment\n");
            return 0;
        }
    }
    if (!map_user_range(USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE,
                        USER_STACK_PAGES * PAGE_SIZE)) {
        uart_printf("elf: OOM mapping stack\n");
        return 0;
    }

    /* Pass 2: copy file contents into the (now stable) mappings.
     * bss tails are already zero — pages were zeroed on allocation. */
    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const elf64_phdr_t *ph = (const elf64_phdr_t *)(blob + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD)
            continue;
        memcpy((void *)ph->p_vaddr, blob + ph->p_offset, ph->p_filesz);
    }

    uart_printf("elf: loaded, entry=%p, user stack top=%p\n",
                (void *)eh->e_entry, (void *)USER_STACK_TOP);
    return eh->e_entry;
}
