/* GDT + TSS. In long mode segmentation is mostly vestigial — CS/SS bases
 * are ignored — but we still need: valid code/data descriptors (Limine's
 * may differ), a TSS so the CPU knows where ring-0 stacks live on a
 * privilege switch, and selectors we can use for syscall/sysret later. */
#ifndef MIRIX_ARCH_GDT_H
#define MIRIX_ARCH_GDT_H

#include <stdint.h>

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
} __attribute__((packed)) gdt_entry_t;

/* 64-bit TSS descriptor takes two entries. */
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit_high;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed)) gdt_tss_entry_t;

/* NOTE: user data MUST precede user code (udata at 0x18, ucode at 0x20).
 * SYSRET loads SS = STAR[63:48]+8 and CS = STAR[63:48]+16, so with the STAR
 * user base 0x10 we get SS=0x18 (udata) and CS=0x20 (ucode). */
typedef struct {
    gdt_entry_t null;       /* 0x00 */
    gdt_entry_t kcode;      /* 0x08 */
    gdt_entry_t kdata;      /* 0x10 */
    gdt_entry_t udata;      /* 0x18 */
    gdt_entry_t ucode;      /* 0x20 */
    gdt_tss_entry_t tss;    /* 0x28 (16 bytes) */
} __attribute__((packed)) gdt_t;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) gdtr_t;

/* Task State Segment — only rsp0 (and ISTs) matter in long mode. */
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint32_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed)) tss_t;

/* Selectors */
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UDATA 0x18
#define SEL_UCODE 0x20
#define SEL_TSS   0x28
#define RPL_USER  3

void gdt_init(void);
void gdt_init_cpu(int cpu);   /* SMP (8c): per-CPU GDT/TSS, slot = lapic id */
void tss_set_rsp0(uint64_t rsp);

#endif
