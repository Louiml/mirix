#include "gdt.h"
#include "cpu.h"
#include "lib/uart.h"

/* Implemented in gdt.asm */
void gdt_flush(gdtr_t *gdtr);
void tss_load(uint16_t selector);

static gdt_t g_gdt;
static tss_t g_tss;
static gdtr_t g_gdtr;

/* Static stacks; Phase 2b's PMM will give us dynamic ones. */
static uint8_t g_kernel_stack[16 * 1024] __attribute__((aligned(16)));
static uint8_t g_df_stack[4 * 1024] __attribute__((aligned(16)));

static void gdt_set_entry(gdt_entry_t *e, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t flags)
{
    e->limit_low = limit & 0xffff;
    e->base_low = base & 0xffff;
    e->base_mid = (base >> 16) & 0xff;
    e->access = access;
    e->flags_limit_high = (flags << 4) | ((limit >> 16) & 0x0f);
    e->base_high = (base >> 24) & 0xff;
}

static void gdt_set_tss(gdt_tss_entry_t *e, uint64_t base, uint32_t limit)
{
    e->limit_low = limit & 0xffff;
    e->base_low = base & 0xffff;
    e->base_mid = (base >> 16) & 0xff;
    e->access = 0x89;                 /* present, ring 0, 64-bit TSS (available) */
    e->flags_limit_high = ((limit >> 16) & 0x0f);  /* G=0: limit is in bytes */
    e->base_high = (base >> 24) & 0xff;
    e->base_upper = (base >> 32) & 0xffffffff;
    e->reserved = 0;
}

void tss_set_rsp0(uint64_t rsp)
{
    g_tss.rsp0 = rsp;
}

void gdt_init(void)
{
    g_gdtr.limit = sizeof(g_gdt) - 1;
    g_gdtr.base = (uint64_t)&g_gdt;

    /* Flat segments, limit 4 GiB (G=1, 4K granularity). In long mode the
     * base/limit are ignored for CS/DS/SS; what matters is the type bits. */
    gdt_set_entry(&g_gdt.null, 0, 0, 0, 0);
    gdt_set_entry(&g_gdt.kcode, 0, 0xfffff, 0x9a, 0xa);  /* exec/read, L=1 (64-bit) */
    gdt_set_entry(&g_gdt.kdata, 0, 0xfffff, 0x92, 0xc);  /* read/write, B=1 */
    gdt_set_entry(&g_gdt.ucode, 0, 0xfffff, 0xfa, 0xa);  /* ring 3 exec/read */
    gdt_set_entry(&g_gdt.udata, 0, 0xfffff, 0xf2, 0xc);  /* ring 3 read/write */
    gdt_set_tss(&g_gdt.tss, (uint64_t)&g_tss, sizeof(g_tss) - 1);

    /* TSS fields */
    g_tss.rsp0 = (uint64_t)g_kernel_stack + sizeof(g_kernel_stack);
    g_tss.ist[0] = (uint64_t)g_df_stack + sizeof(g_df_stack);  /* IST1: #DF stack */
    g_tss.iopb_offset = sizeof(g_tss);

    gdt_flush(&g_gdtr);   /* lgdt + reload CS=0x08, data segs=0x10 */
    tss_load(SEL_TSS);    /* ltr 0x28 */
    uart_printf("gdt: loaded (kcode=%p kdata=%p tss=%p), rsp0=%p\n",
                (void *)(uint64_t)SEL_KCODE, (void *)(uint64_t)SEL_KDATA,
                (void *)(uint64_t)SEL_TSS, (void *)g_tss.rsp0);
}
