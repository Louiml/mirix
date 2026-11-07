/* ACPI tables are in low memory; access them through the HHDM. */
#include "acpi.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "lib/io.h"
#include "mm/mm.h"
#include "arch/x86_64/cpu.h"

typedef struct {
    char     sig[8];
    uint8_t  checksum;
    char     oemid[6];
    uint8_t  revision;
    uint32_t rsdt_addr;
    uint32_t length;          /* ACPI 2.0+ */
    uint64_t xsdt_addr;
    uint8_t  ext_checksum;
    uint8_t  reserved[3];
} __attribute__((packed)) rsdp_t;

typedef struct {
    char     sig[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oemid[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed)) sdt_header_t;

static uint16_t g_pm1a_cnt;     /* PM1a_CNT_BLK port, 0 = none */
static uint8_t  g_pm1_cnt_len;
static uint8_t  g_slp_typ;      /* S5 SLP_TYP from the DSDT _S5 package */
static int      g_ready;
static sdt_header_t *g_xsdt;   /* Phase 9b: kept for acpi_find_table */
static int g_xsd;
static uint64_t g_n;

/* Scan DSDT AML for Name(_S5, Package(...)) = 08 5F 53 35 5F 12 <pkglen>
 * <numelems> <elem...>; the first ByteConst (0A xx) is SLP_TYPa. */
static uint8_t dsdt_slp_typ(const uint8_t *aml, uint32_t len)
{
    static const uint8_t pat[6] = { 0x08, 0x5f, 0x53, 0x35, 0x5f, 0x12 };
    for (uint32_t i = 0; i + 10 < len; i++) {
        if (memcmp(aml + i, pat, 6) != 0)
            continue;
        uint32_t j = i + 6;
        /* PkgLength: 1 byte if top 2 bits are 0, else 2-4 bytes (count in
         * top 2 bits of first byte, +1 length bytes follow) */
        uint8_t b = aml[j];
        uint32_t plen_bytes = 1;
        if ((b & 0xc0) != 0)
            plen_bytes = 1 + ((b >> 6) & 0x3);
        j += plen_bytes;                 /* skip PkgLength */
        j++;                             /* NumElements */
        for (uint32_t k = 0; k < 64 && j + 2 <= len; k++) {
            if (aml[j] == 0x0a)          /* ByteConst */
                return aml[j + 1];
            if (aml[j] == 0x00)          /* Zero */
                return 0;
            j += 2;                      /* skip unknown element (approx) */
        }
        return 0;
    }
    return 0;
}

int acpi_init(uint64_t rsdp_phys)
{
    rsdp_t *rsdp = (rsdp_t *)phys_to_virt(rsdp_phys);
    if (!rsdp || memcmp(rsdp->sig, "RSD PTR ", 8) != 0) {
        uart_printf("acpi: bad RSDP signature at phys %p\n", (void *)rsdp_phys);
        return -1;
    }
    uint64_t tbl_phys = (rsdp->revision >= 2) ? rsdp->xsdt_addr : rsdp->rsdt_addr;
    sdt_header_t *xsdt = (sdt_header_t *)phys_to_virt(tbl_phys);
    if (!xsdt) {
        uart_printf("acpi: XSDT phys 0x%llx unmapped\n", (unsigned long long)tbl_phys);
        return -2;
    }
    int xsd = (memcmp(xsdt->sig, "XSDT", 4) == 0);
    if (!xsd && memcmp(xsdt->sig, "RSDT", 4) != 0) {
        uart_printf("acpi: bad table signature '%.4s'\n", xsdt->sig);
        return -3;
    }
    int esz = xsd ? 8 : 4;
    uint64_t n = (xsdt->length - sizeof(sdt_header_t)) / esz;
    g_xsdt = xsdt; g_xsd = xsd; g_n = n;
    uart_printf("acpi: %s at phys %p len %u -> %llu tables\n",
                xsd ? "XSDT" : "RSDT", (void *)tbl_phys,
                xsdt->length, (unsigned long long)n);
    /* ACPI tables always live inside RAM; skip entries outside the first
     * 1 GiB so a garbage pointer can't fault the HHDM read. */
    sdt_header_t *fadt = 0;
    uint64_t dsdt_phys = 0;
    for (uint64_t i = 0; i < n; i++) {
        uint64_t tp = xsd
            ? *(uint64_t *)((uint8_t *)xsdt + sizeof(sdt_header_t) + i * 8)
            : *(uint32_t *)((uint8_t *)xsdt + sizeof(sdt_header_t) + i * 4);
        if (tp >= 0x40000000ULL)
            continue;
        sdt_header_t *t = (sdt_header_t *)phys_to_virt(tp);
        if (memcmp(t->sig, "FACP", 4) == 0)
            fadt = t;
    }
    if (!fadt) {
        uart_printf("acpi: no FADT in %s (%llu tables)\n", xsd ? "XSDT" : "RSDT",
                    (unsigned long long)n);
        return -4;
    }
    uart_printf("acpi: FADT at phys %p len %u rev %d\n",
                (void *)((uint64_t)fadt - g_hhdm_offset),
                fadt->length, fadt->revision);
    uint8_t *f = (uint8_t *)fadt;
    /* PM1a_CNT: ACPI 2.0+ puts a GAS at +80 (address at +84); OVMF's rev-3
     * FADT here uses the legacy 32-bit I/O port field at +64. Try both and
     * keep the first sane I/O port (< 0x10000, nonzero). */
    uint64_t pm1a_addr = *(uint32_t *)(f + 64);
    if ((pm1a_addr == 0 || pm1a_addr >= 0x10000ULL) && fadt->length >= 92)
        pm1a_addr = *(uint64_t *)(f + 80 + 4);
    g_pm1a_cnt = (uint16_t)(pm1a_addr & 0xffff);
    /* PM1_CNT_LEN: legacy byte at +89, modern at +153 */
    g_pm1_cnt_len = f[89];
    if (!g_pm1_cnt_len && fadt->length >= 154)
        g_pm1_cnt_len = f[153];
    /* DSDT: legacy 32-bit phys at +40; modern X_DSDT (8 bytes) at +204 */
    dsdt_phys = *(uint32_t *)(f + 40);
    if ((!dsdt_phys || dsdt_phys >= 0x40000000ULL) && fadt->length >= 212) {
        uint64_t xd = *(uint64_t *)(f + 204);
        if (xd && xd < 0x40000000ULL)
            dsdt_phys = xd;
    }
    g_slp_typ = 0;
    uart_printf("acpi: PM1a_CNT raw addr=0x%llx, PM1_CNT_LEN=%d, DSDT phys=0x%llx\n",
                (unsigned long long)pm1a_addr, g_pm1_cnt_len,
                (unsigned long long)dsdt_phys);
    if (dsdt_phys && dsdt_phys < 0x40000000ULL) {
        sdt_header_t *d = (sdt_header_t *)phys_to_virt(dsdt_phys);
        if (memcmp(d->sig, "DSDT", 4) == 0)
            g_slp_typ = dsdt_slp_typ((const uint8_t *)d + sizeof(sdt_header_t),
                                     d->length - sizeof(sdt_header_t));
        else
            uart_printf("acpi: DSDT sig is '%.4s'?!\n", d->sig);
    }
    g_ready = 1;
    uart_printf("acpi: FADT rev %d, PM1a_CNT port 0x%x len %d, _S5 SLP_TYP=%d (%s)\n",
                fadt->revision, g_pm1a_cnt, g_pm1_cnt_len, g_slp_typ,
                dsdt_phys ? "DSDT scanned" : "no DSDT, default 0");
    return 0;
}

void acpi_poweroff(void)
{
    if (!g_ready || !g_pm1a_cnt) {
        uart_printf("acpi: poweroff FAILED (no PM1a_CNT)\n");
        return;
    }
    cli();
    uart_printf("acpi: poweroff -> S5 via PM1a_CNT port 0x%x (SLP_TYP=%d | SLP_EN)\n",
                g_pm1a_cnt, g_slp_typ);
    sti();
    /* PM1_CNT: SLP_TYP in bits 10-12, SLP_EN = bit 13 */
    outw(g_pm1a_cnt, (uint16_t)(((uint16_t)g_slp_typ << 10) | (1u << 13)));
    /* Should not return — if it does, retry once then give up. */
    for (volatile uint64_t i = 0; i < 50000000ULL; i++)
        ;
    outw(g_pm1a_cnt, (uint16_t)(((uint16_t)g_slp_typ << 10) | (1u << 13)));
    for (volatile uint64_t i = 0; i < 50000000ULL; i++)
        ;
}

/* Phase 9b: generic table lookup (the IO-APIC driver uses this to find the
 * MADT, signature "APIC"). Returns the table via the HHDM, or 0 if absent. */
const void *acpi_find_table(const char *sig, uint32_t *len_out)
{
    if (!g_xsdt)
        return 0;
    int esz = g_xsd ? 8 : 4;
    for (uint64_t i = 0; i < g_n; i++) {
        uint64_t tp = g_xsd
            ? *(uint64_t *)((uint8_t *)g_xsdt + sizeof(sdt_header_t) + i * 8)
            : *(uint32_t *)((uint8_t *)g_xsdt + sizeof(sdt_header_t) + i * 4);
        if (tp >= 0x40000000ULL)
            continue;
        sdt_header_t *t = (sdt_header_t *)phys_to_virt(tp);
        if (memcmp(t->sig, sig, 4) == 0) {
            if (len_out)
                *len_out = t->length;
            return t;
        }
    }
    return 0;
}
