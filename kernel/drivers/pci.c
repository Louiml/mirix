#include "pci.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/uart.h"

/* q35 chipset PCIe ECAM (MMCONFIG). The memmap shows the 256 MiB reserved
 * ECAM hole at 0xE0000000 (OVMF relocates MMCONFIG there; the classic
 * 0xB0000000 base reads as zeros here). We only scan bus 0, so map just
 * the first 1 MiB into a kernel VA. */
#define PCI_ECAM_PHYS 0xE0000000ULL
#define PCI_ECAM_VIRT 0xffffffffe0000000ULL
#define PCI_ECAM_BUS0_BYTES 0x100000ULL   /* 1 MiB covers bus 0 */

static pci_dev_t g_devs[64];
static int g_ndevs;

static uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint64_t addr = PCI_ECAM_VIRT +
                    ((uint32_t)bus << 20 | (uint32_t)dev << 15 |
                     (uint32_t)func << 12 | off);
    return *(volatile uint32_t *)addr;
}

static void pci_map_ecam(void)
{
    for (uint64_t i = 0; i < PCI_ECAM_BUS0_BYTES; i += PAGE_SIZE)
        vmm_map(PCI_ECAM_VIRT + i, PCI_ECAM_PHYS + i, PTE_PRESENT | PTE_RW);
}

uint32_t pci_conf_read32(const pci_dev_t *d, uint8_t off)
{
    return pci_read32(d->bus, d->dev, d->func, off);
}

void pci_conf_write32(const pci_dev_t *d, uint8_t off, uint32_t v)
{
    uint64_t addr = PCI_ECAM_VIRT +
                    ((uint32_t)d->bus << 20 | (uint32_t)d->dev << 15 |
                     (uint32_t)d->func << 12 | off);
    *(volatile uint32_t *)addr = v;
}

void pci_enable_mem_busmaster(const pci_dev_t *d)
{
    uint32_t cmd = pci_conf_read32(d, 0x04);
    cmd |= 0x06;   /* bit1 memory space, bit2 bus master */
    pci_conf_write32(d, 0x04, cmd);
}

int pci_count(void) { return g_ndevs; }
const pci_dev_t *pci_devices(void) { return g_devs; }

const pci_dev_t *pci_find_by_class(uint8_t class, uint8_t subclass)
{
    for (int i = 0; i < g_ndevs; i++)
        if (g_devs[i].class == class && g_devs[i].subclass == subclass)
            return &g_devs[i];
    return 0;
}

void pci_init(void)
{
    pci_map_ecam();
    g_ndevs = 0;

    for (int dev = 0; dev < 32; dev++) {
        uint32_t id0 = pci_read32(0, dev, 0, 0);
        if ((id0 & 0xffff) == 0xffff)
            continue;   /* no device */
        uint8_t hdr = (pci_read32(0, dev, 0, 0x0c) >> 16) & 0xff;
        int max_func = (hdr & 0x80) ? 8 : 1;
        for (int func = 0; func < max_func; func++) {
            uint32_t id = pci_read32(0, dev, func, 0);
            if ((id & 0xffff) == 0xffff)
                continue;
            if (g_ndevs >= 64)
                break;
            pci_dev_t *d = &g_devs[g_ndevs++];
            d->bus = 0;
            d->dev = (uint8_t)dev;
            d->func = (uint8_t)func;
            d->vendor = (uint16_t)(id & 0xffff);
            d->device = (uint16_t)(id >> 16);
            uint32_t creg = pci_read32(0, dev, func, 0x08);
            d->rev = (uint8_t)(creg & 0xff);
            d->progif = (uint8_t)(creg >> 8);
            d->subclass = (uint8_t)(creg >> 16);
            d->class = (uint8_t)(creg >> 24);
            d->header_type = hdr;
            for (int b = 0; b < 6; b++)
                d->bar[b] = pci_read32(0, dev, func, 0x10 + b * 4);
            uart_printf("pci: %02x:%02x.%d %04x:%04x class=%02x.%02x.%02x rev=%02x bar0=%08x bar5=%08x\n",
                        d->bus, d->dev, d->func, d->vendor, d->device,
                        d->class, d->subclass, d->progif, d->rev,
                        d->bar[0], d->bar[5]);
        }
    }
    uart_printf("pci: enumerated %d devices (ECAM phys %p, virt %p)\n",
                g_ndevs, (void *)PCI_ECAM_PHYS, (void *)PCI_ECAM_VIRT);
}
