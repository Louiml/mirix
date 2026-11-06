/* Intel 82540EM (e1000) NIC driver — MMIO BAR0, polling RX/TX (no IRQ).
 * QEMU: -netdev user,id=net0 -device e1000,netdev=net0 (slirp answers ARP
 * for the gateway 10.0.2.2, which is what the boot test uses). */
#ifndef MIRIX_DRIVERS_E1000_H
#define MIRIX_DRIVERS_E1000_H

#include <stdint.h>
#include <stddef.h>

int  e1000_init(void);                 /* 0 on success */
int  e1000_send(const void *frame, uint16_t len);   /* 0 on success */
long e1000_recv(void *buf, long maxlen);            /* bytes, 0 if none (poll) */
void e1000_get_mac(uint8_t mac[6]);

#endif
