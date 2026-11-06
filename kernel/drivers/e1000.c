/* Intel 82540EM (e1000) driver: BAR0 MMIO, descriptor rings in contiguous
 * PMM, polled (no IRQ). Registers per the 82540EM datasheet. */
#include "e1000.h"
#include "pci.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "mm/pmm.h"
#include "mm/mm.h"
#include "mm/vmm.h"
#include "drivers/timer.h"

#define E1000_VIRT 0xffffffffe2000000ULL   /* 128 KiB of BAR0 mapped here */

/* registers (offsets into BAR0) */
#define REG_CTRL   0x0000
#define REG_STATUS 0x0008
#define REG_ICR    0x00C0
#define REG_IMS    0x00D0
#define REG_IMC    0x00D8
#define REG_RCTL   0x0100
#define REG_TCTL   0x0400
#define REG_RDBAL  0x2800
#define REG_RDBAH  0x2804
#define REG_RDLEN  0x2808
#define REG_RDH    0x2810
#define REG_RDT    0x2818
#define REG_MTA    0x5200   /* 128 x u32 */
#define REG_RAL0   0x5400
#define REG_RAH0   0x5404
#define REG_TDBAL  0x3800
#define REG_TDBAH  0x3804
#define REG_TDLEN  0x3808
#define REG_TDH    0x3810
#define REG_TDT    0x3818

#define CTRL_RST (1u << 26)
#define CTRL_FD  (1u << 0)
#define CTRL_SLU (1u << 6)
#define RCTL_EN    (1u << 1)
#define RCTL_BAM   (1u << 15)
#define RCTL_SECRC (1u << 26)
#define TCTL_EN (1u << 1)
#define TCTL_PSP (1u << 3)

#define NUM_RX 16
#define NUM_TX 8
#define PKT_BUF 2048

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} e1000_desc_t;

static volatile uint32_t *g_regs;
static uint8_t g_mac[6];
static int g_ok;

static e1000_desc_t *g_rx_desc;   /* HHDM view */
static e1000_desc_t *g_tx_desc;
static uint64_t g_rx_desc_phys, g_tx_desc_phys;
static uint64_t g_rx_bufs_phys, g_tx_bufs_phys;   /* NUM * PKT_BUF contiguous */
static int g_rx_idx, g_tx_idx;

static inline uint32_t rd32(uint32_t off) { return g_regs[off / 4]; }
static inline void wr32(uint32_t off, uint32_t v) { g_regs[off / 4] = v; }

int e1000_init(void)
{
    const pci_dev_t *d = 0;
    for (int i = 0; i < pci_count(); i++) {
        const pci_dev_t *p = &pci_devices()[i];
        if (p->vendor == 0x8086 && p->device == 0x10d3) { d = p; break; }
    }
    if (!d)
        return -1;
    pci_enable_mem_busmaster(d);
    uint64_t bar0 = d->bar[0] & ~0xfULL;
    if (!bar0)
        return -2;
    for (int i = 0; i < 32; i++)   /* 128 KiB */
        vmm_map(E1000_VIRT + i * PAGE_SIZE, bar0 + i * PAGE_SIZE,
                PTE_PRESENT | PTE_RW);
    g_regs = (volatile uint32_t *)E1000_VIRT;

    wr32(REG_IMC, 0xffffffff);            /* mask all interrupts */
    wr32(REG_CTRL, rd32(REG_CTRL) | CTRL_RST);
    msleep(20);
    wr32(REG_CTRL, CTRL_FD | CTRL_SLU);
    (void)rd32(REG_ICR);                  /* clear pending */

    /* MAC from RAL0/RAH0 */
    uint32_t ral = rd32(REG_RAL0), rah = rd32(REG_RAH0);
    for (int i = 0; i < 4; i++) g_mac[i] = (uint8_t)(ral >> (8 * i));
    g_mac[4] = (uint8_t)(rah >> 0);
    g_mac[5] = (uint8_t)(rah >> 8);

    /* clear multicast table */
    for (int i = 0; i < 128; i++)
        wr32(REG_MTA + i * 4, 0);

    /* RX ring: descriptors in one page, buffers in NUM_RX*PKT_BUF contiguous */
    g_rx_desc_phys = pmm_alloc_page();
    g_rx_bufs_phys = pmm_alloc_pages_contig(NUM_RX * PKT_BUF / PAGE_SIZE);
    g_tx_desc_phys = pmm_alloc_page();
    g_tx_bufs_phys = pmm_alloc_pages_contig(NUM_TX * PKT_BUF / PAGE_SIZE);
    if (!g_rx_desc_phys || !g_rx_bufs_phys || !g_tx_desc_phys || !g_tx_bufs_phys)
        return -3;
    g_rx_desc = (e1000_desc_t *)phys_to_virt(g_rx_desc_phys);
    g_tx_desc = (e1000_desc_t *)phys_to_virt(g_tx_desc_phys);
    memset(g_rx_desc, 0, PAGE_SIZE);
    memset(g_tx_desc, 0, PAGE_SIZE);
    for (int i = 0; i < NUM_RX; i++) {
        g_rx_desc[i].addr = g_rx_bufs_phys + (uint64_t)i * PKT_BUF;
        g_rx_desc[i].status = 0;
    }
    wr32(REG_RDBAL, (uint32_t)g_rx_desc_phys);
    wr32(REG_RDBAH, (uint32_t)(g_rx_desc_phys >> 32));
    wr32(REG_RDLEN, NUM_RX * sizeof(e1000_desc_t));
    wr32(REG_RDH, 0);
    g_rx_idx = 0;
    wr32(REG_RDT, NUM_RX - 1);
    wr32(REG_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* BSIZE=2048 (bits 16-17 = 0) */

    /* TX ring */
    for (int i = 0; i < NUM_TX; i++)
        g_tx_desc[i].addr = g_tx_bufs_phys + (uint64_t)i * PKT_BUF;
    wr32(REG_TDBAL, (uint32_t)g_tx_desc_phys);
    wr32(REG_TDBAH, (uint32_t)(g_tx_desc_phys >> 32));
    wr32(REG_TDLEN, NUM_TX * sizeof(e1000_desc_t));
    wr32(REG_TDH, 0);
    g_tx_idx = 0;
    wr32(REG_TDT, 0);
    wr32(REG_TCTL, TCTL_EN | TCTL_PSP | (0x10u << 4) | (0x40u << 12));

    g_ok = 1;
    uart_printf("e1000: 8086:10d3 at %02x:%02x.%d, BAR0 phys %p, mac=%02x:%02x:%02x:%02x:%02x:%02x\n",
                d->bus, d->dev, d->func, (void *)bar0,
                g_mac[0], g_mac[1], g_mac[2], g_mac[3], g_mac[4], g_mac[5]);
    uart_printf("e1000: RX/TX rings ready (%d/%d descs, polling)\n", NUM_RX, NUM_TX);
    return 0;
}

void e1000_get_mac(uint8_t mac[6])
{
    if (g_ok)
        memcpy(mac, g_mac, 6);
}

static int e1000_send_locked(const void *frame, uint16_t len)
{
    if (!g_ok || len > PKT_BUF)
        return -1;
    int i = g_tx_idx;
    uint8_t *buf = (uint8_t *)phys_to_virt(g_tx_bufs_phys + (uint64_t)i * PKT_BUF);
    memcpy(buf, frame, len);
    e1000_desc_t *d = &g_tx_desc[i];
    d->addr = g_tx_bufs_phys + (uint64_t)i * PKT_BUF;
    d->length = len;
    d->csum = 0;
    /* TX cmd byte lives at offset 11 (EOP | IFCS | RS) — in the shared
     * descriptor struct that's the high byte of `csum` */
    ((uint8_t *)d)[11] = 0x01 | 0x02 | 0x08;
    d->status = 0;
    wr32(REG_TDT, (uint32_t)((i + 1) % NUM_TX));
    /* poll for DD (bit 0 of status) — up to ~200 ms */
    for (int t = 0; t < 2000; t++) {
        if (d->status & 0x01)
            break;
        msleep(1);
    }
    g_tx_idx = (i + 1) % NUM_TX;
    return (d->status & 0x01) ? 0 : -2;
}

static long e1000_recv_locked(void *buf, long maxlen)
{
    if (!g_ok)
        return -1;
    e1000_desc_t *d = &g_rx_desc[g_rx_idx];
    if (!(d->status & 0x01))          /* DD not set: no packet */
        return 0;
    long n = d->length;
    if (n > maxlen)
        n = maxlen;
    memcpy(buf, phys_to_virt(d->addr), (size_t)n);
    d->status = 0;
    g_rx_idx = (g_rx_idx + 1) % NUM_RX;
    wr32(REG_RDT, (uint32_t)g_rx_idx);
    return n;
}

/* SMP (8c): one lock guards the TX/RX rings — a leaf lock (send polls with
 * msleep, so it must NOT be taken while holding sched/vfs/wm). */
#include "lib/spinlock.h"
static spinlock_t g_net_lock = SPINLOCK_INIT;

int e1000_send(const void *frame, uint16_t len)
{
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    int r = e1000_send_locked(frame, len);
    spin_unlock_irqrestore(&g_net_lock, fl);
    return r;
}

long e1000_recv(void *buf, long maxlen)
{
    uint64_t fl = spin_lock_irqsave(&g_net_lock);
    long r = e1000_recv_locked(buf, maxlen);
    spin_unlock_irqrestore(&g_net_lock, fl);
    return r;
}
