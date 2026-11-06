/* AHCI (SATA) driver — minimal: finds the first disk on the ICH9 AHCI,
 * brings one port up, and reads sectors with READ DMA EXT (polling).
 * The block device is exposed to userland via SYS_diskread. */
#ifndef MIRIX_DRIVERS_AHCI_H
#define MIRIX_DRIVERS_AHCI_H

#include <stdint.h>

#define AHCI_MAX_SECTORS_PER_READ 64   /* bounce buffer is 64 sectors */

int  ahci_init(void);                                  /* 0 = a disk port is up */
int  ahci_port_online(void);                           /* 1 if ahci_init found a disk */
uint32_t ahci_sector_size(void);                       /* 512 */
/* Read nsec (<= AHCI_MAX_SECTORS_PER_READ) sectors at lba into kvaddr
 * (a kernel/HHDM virtual address). Returns bytes read, 0 on error. */
long ahci_read(uint64_t lba, uint32_t nsec, void *kvaddr);

#endif
