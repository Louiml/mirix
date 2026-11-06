/* PCI enumeration via MMCONFIG/ECAM (q35: ECAM at phys 0xB0000000).
 * Bus 0 scan; bridges are followed no further (q35 puts all built-in
 * devices on bus 0). Devices are kept in a small table for later drivers
 * (AHCI, NIC, virtio...) to find by class. */
#ifndef MIRIX_DRIVERS_PCI_H
#define MIRIX_DRIVERS_PCI_H

#include <stdint.h>

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, progif, rev;
    uint8_t  header_type;
    uint32_t bar0;
} pci_dev_t;

void pci_init(void);
int  pci_count(void);
const pci_dev_t *pci_devices(void);
const pci_dev_t *pci_find_by_class(uint8_t class, uint8_t subclass);

#endif
