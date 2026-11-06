/* Minimal AHCI driver for QEMU's ICH9 AHCI (8086:2922, class 01.06.01).
 *
 * Brings up port 0 (or the first port with a SATA disk attached) with
 * polling, and implements READ DMA EXT (ATA cmd 0x25) into a contiguous
 * PMM bounce buffer. All DMA structures (command list, FIS receive area,
 * command table, data) live in ONE physically-contiguous 96 KiB block so
 * the HBA can reach them via physical addresses:
 *   +0x0000  command list   (1 KiB, 32 slots x 32 B)
 *   +0x0400  FIS receive    (256 B)
 *   +0x1000  command tables (32 x 128 B = 4 KiB)
 *   +0x2000  data bounce    (up to 92 KiB -> 64 sectors max per read)
 */
#include "ahci.h"
#include "pci.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "drivers/timer.h"

#define AHCI_VIRT       0xffffffffe1000000ULL   /* 4 KiB of ABAR mapped here */
#define AHCI_PORT_REGS  0x80
#define AHCI_MAX_PORTS  6                        /* ICH9 has 6 */

/* HBA register offsets */
#define HBA_GHC   0x00
#define HBA_PI    0x0c
#define HBA_VS    0x08

/* Port register offsets (base = ABAR + 0x100 + port*0x80) */
#define PxCLB   0x00
#define PxCLBU  0x04
#define PxFB    0x08
#define PxFBU   0x0c
#define PxIS    0x10
#define PxIE    0x14
#define PxCMD   0x18
#define PxTFD   0x1c
#define PxSIG   0x20
#define PxSSTS  0x24
#define PxSCTL  0x28
#define PxSERR  0x2c
#define PxSACT  0x30
#define PxCI    0x34

#define PxCMD_ST  0x01
#define PxCMD_FRE 0x10
#define PxCMD_FR  0x20
#define PxCMD_CR  0x8000
#define PxCMD_CLO 0x08

#define PxSSTS_DET_MASK 0x0f
#define PxSSTS_DET_OK   0x03

#define PxTFD_BSY 0x80
#define PxTFD_DRQ 0x08

#define PxIS_TFES (1u << 30)

#define ATA_CMD_READ_DMA_EXT 0x25

#define PORT_BLOCK_PAGES 24                       /* 96 KiB */
#define PORT_DATA_OFF    0x2000
#define PORT_DATA_MAX    (PORT_BLOCK_PAGES * PAGE_SIZE - PORT_DATA_OFF)

typedef struct {
    volatile uint32_t clb, clbu, fb, fbu;       /* 0x00-0x0c */
    volatile uint32_t is, ie, cmd;              /* 0x10-0x18 */
    volatile uint32_t rsv0;                     /* 0x1c reserved */
    volatile uint32_t tfd, sig, ssts, sctl, serr, sact, ci, sntf, fbs; /* 0x20-0x40 */
    uint32_t rsv[15];                           /* pad to the 0x80-byte port stride */
} hba_port_t;

typedef struct {
    volatile uint32_t cap, ghc, is, pi, vs, ccc_ctl, ccc_pts, em_loc;
    volatile uint32_t em_ctl, cap2, bohc;
    uint8_t rsv[0x74];
    uint8_t vendor[0x60];
    hba_port_t ports[32];
} hba_mem_t;

typedef struct {
    uint8_t cfl : 5;
    uint8_t a   : 1;
    uint8_t w   : 1;
    uint8_t p   : 1;
    uint8_t r   : 1;
    uint8_t b   : 1;
    uint8_t c   : 1;
    uint8_t rsv0: 1;
    uint8_t pmp : 4;
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv1[4];
} hba_cmd_header_t;

typedef struct {
    uint32_t dba;
    uint32_t dbau;
    uint32_t rsv;
    uint32_t dbc;      /* byte count - 1, bits 0-21 */
} hba_prdt_entry_t;

typedef struct {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    hba_prdt_entry_t prdt[8];
} hba_cmd_table_t;

static volatile hba_mem_t *g_hba;
static uint64_t g_port_phys;          /* contiguous block, phys */
static int g_port;                    /* active port index, -1 = none */

static volatile hba_port_t *port_regs(int p)
{
    return &g_hba->ports[p];
}

static int wait_clear(volatile uint32_t *reg, uint32_t mask, int ms)
{
    for (int i = 0; i < ms; i++) {
        if ((*reg & mask) == 0)
            return 1;
        msleep(1);
    }
    return (*reg & mask) == 0;
}

static int wait_set(volatile uint32_t *reg, uint32_t mask, int ms)
{
    for (int i = 0; i < ms; i++) {
        if ((*reg & mask) == mask)
            return 1;
        msleep(1);
    }
    return (*reg & mask) == mask;
}

int ahci_port_online(void) { return g_port >= 0; }
uint32_t ahci_sector_size(void) { return 512; }

int ahci_init(void)
{
    g_port = -1;

    const pci_dev_t *d = pci_find_by_class(0x01, 0x06);   /* mass storage, SATA */
    if (!d) {
        uart_printf("ahci: no SATA controller found\n");
        return -1;
    }
    if (d->bar[5] == 0 || (d->bar[5] & 0xf) != 0) {
        uart_printf("ahci: bad ABAR (bar5=%08x)\n", d->bar[5]);
        return -1;
    }
    uint64_t abar_phys = d->bar[5] & ~0xfULL;
    pci_enable_mem_busmaster(d);

    for (uint64_t i = 0; i < 0x1000; i += PAGE_SIZE)
        vmm_map(AHCI_VIRT + i, abar_phys + i, PTE_PRESENT | PTE_RW);
    g_hba = (volatile hba_mem_t *)AHCI_VIRT;

    uart_printf("ahci: %04x:%04x at %02x:%02x.%d, ABAR phys %p (virt %p), vs %08x, pi %08x\n",
                d->vendor, d->device, d->bus, d->dev, d->func,
                (void *)abar_phys, (void *)AHCI_VIRT,
                g_hba->vs, g_hba->pi);

    /* QEMU's AHCI swaps the GHC bits vs the AHCI spec: bit0 = HR (host reset,
     * self-clearing; ahci_reset() leaves bit31 set), bit31 = AE (AHCI enable).
     * On real spec-compliant hardware bit0 = AE. So: write 1 -> QEMU pulses
     * a reset and ends with AE set; real HW just sets AE. Either way one of
     * the two "enabled" bits reads back set afterwards. IE (bit1) stays 0:
     * we poll, no interrupts. */
    g_hba->ghc = 0x1;
    msleep(50);
    if ((g_hba->ghc & 0x80000001) == 0) {
        uart_printf("ahci: not enabled after reset (ghc=%08x)\n", g_hba->ghc);
        return -1;
    }

    /* Find a port with a device attached (DET == 3). */
    int found = -1;
    uint32_t pi = g_hba->pi;
    for (int p = 0; p < AHCI_MAX_PORTS; p++) {
        if (!(pi & (1u << p)))
            continue;
        volatile hba_port_t *pr = port_regs(p);
        if ((pr->ssts & PxSSTS_DET_MASK) != PxSSTS_DET_OK)
            continue;
        uart_printf("ahci: port %d has device, sig=%08x ssts=%08x\n",
                    p, pr->sig, pr->ssts);
        found = p;
        break;
    }
    if (found < 0) {
        uart_printf("ahci: no disk attached (pi=%08x)\n", pi);
        return -1;
    }
    g_port = found;
    volatile hba_port_t *pr = port_regs(g_port);

    /* Stop the command engine before touching CLB/FB. */
    pr->cmd &= ~PxCMD_ST;
    if (!wait_clear(&pr->cmd, PxCMD_CR, 500)) {
        uart_printf("ahci: port %d CR did not clear\n", g_port);
        return -1;
    }
    pr->cmd &= ~PxCMD_FRE;
    wait_clear(&pr->cmd, PxCMD_FR, 500);

    /* One contiguous block for CL + FIS + CT + data. */
    g_port_phys = pmm_alloc_pages_contig(PORT_BLOCK_PAGES);
    if (!g_port_phys) {
        uart_printf("ahci: no %d contiguous pages\n", PORT_BLOCK_PAGES);
        return -1;
    }
    uint8_t *blk = phys_to_virt(g_port_phys);
    memset(blk, 0, PORT_BLOCK_PAGES * PAGE_SIZE);

    uint64_t cl_phys  = g_port_phys + 0x0000;
    uint64_t fb_phys  = g_port_phys + 0x0400;
    uint64_t ct_phys  = g_port_phys + 0x1000;

    pr->clb  = (uint32_t)cl_phys;
    pr->clbu = (uint32_t)(cl_phys >> 32);
    pr->fb   = (uint32_t)fb_phys;
    pr->fbu  = (uint32_t)(fb_phys >> 32);

    /* Clear pending errors/interrupts. */
    pr->serr = 0xffffffff;
    pr->is   = 0xffffffff;
    pr->ie   = 0;

    /* Start FIS receive, then the command engine. */
    pr->cmd |= PxCMD_FRE;
    wait_set(&pr->cmd, PxCMD_FR, 500);
    pr->cmd |= PxCMD_ST;

    hba_cmd_header_t *cl = (hba_cmd_header_t *)phys_to_virt(cl_phys);
    memset(cl, 0, 32 * sizeof(hba_cmd_header_t));
    cl[0].cfl   = 5;            /* 20-byte command FIS */
    cl[0].w     = 0;            /* read: D2H */
    cl[0].prdtl = 8;
    cl[0].ctba  = (uint32_t)ct_phys;
    cl[0].ctbau = (uint32_t)(ct_phys >> 32);

    uart_printf("ahci: port %d online (cl=%p fb=%p ct=%p, block phys %p)\n",
                g_port, (void *)cl_phys, (void *)fb_phys, (void *)ct_phys,
                (void *)g_port_phys);
    return 0;
}

long ahci_read(uint64_t lba, uint32_t nsec, void *kvaddr)
{
    if (g_port < 0 || nsec == 0 || nsec * 512 > PORT_DATA_MAX)
        return 0;
    if ((uint64_t)kvaddr < g_hhdm_offset)
        return 0;   /* need a kernel/HHDM address */

    volatile hba_port_t *pr = port_regs(g_port);
    uint64_t cl_phys = g_port_phys;
    uint64_t ct_phys = g_port_phys + 0x1000;
    uint64_t data_phys = g_port_phys + PORT_DATA_OFF;

    hba_cmd_header_t *cl = (hba_cmd_header_t *)phys_to_virt(cl_phys);
    hba_cmd_table_t *ct = (hba_cmd_table_t *)phys_to_virt(ct_phys);

    /* Build the H2D FIS: READ DMA EXT. */
    memset(ct, 0, sizeof(hba_cmd_table_t));
    uint8_t *cfis = ct->cfis;
    cfis[0] = 0x27;                 /* FIS type: register H2D */
    cfis[1] = 0x80;                 /* C=1 */
    cfis[2] = ATA_CMD_READ_DMA_EXT;
    cfis[3] = 0;                    /* features low */
    cfis[4] = (uint8_t)(lba);
    cfis[5] = (uint8_t)(lba >> 8);
    cfis[6] = (uint8_t)(lba >> 16);
    cfis[7] = 0x40;                 /* device: LBA mode */
    cfis[8] = (uint8_t)(lba >> 24);
    cfis[9] = (uint8_t)(lba >> 32);
    cfis[10] = (uint8_t)(lba >> 40);
    cfis[11] = 0;                   /* features high */
    cfis[12] = (uint8_t)(nsec);
    cfis[13] = (uint8_t)(nsec >> 8);

    /* One PRDT entry pointing at the bounce buffer. */
    ct->prdt[0].dba  = (uint32_t)data_phys;
    ct->prdt[0].dbau = (uint32_t)(data_phys >> 32);
    ct->prdt[0].dbc  = nsec * 512 - 1;
    cl[0].prdtl = 1;
    cl[0].prdbc = 0;

    /* Issue on slot 0 and poll. */
    pr->is = 0xffffffff;            /* clear status */
    pr->ci = 1;                     /* slot 0 */
    int ok = 0;
    for (int i = 0; i < 3000; i++) {
        if ((pr->ci & 1) == 0) { ok = 1; break; }
        if (pr->is & PxIS_TFES) break;
        msleep(1);
    }
    if (!ok || (pr->is & PxIS_TFES) || (pr->tfd & (PxTFD_BSY | PxTFD_DRQ))) {
        uart_printf("ahci: read LBA %llu FAILED (ci=%08x is=%08x tfd=%02x)\n",
                    (unsigned long long)lba, pr->ci, pr->is,
                    (uint8_t)(pr->tfd & 0xff));
        return 0;
    }

    memcpy(kvaddr, phys_to_virt(data_phys), (size_t)nsec * 512);
    return (long)nsec * 512;
}
