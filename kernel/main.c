/* MirixOS kernel entry — Phase 0 stub.
 *
 * Limine drops us here in 64-bit long mode with paging on, a stack, and a
 * higher-half mapping of the kernel. All we do right now is prove the boot
 * chain works: say hello over the UART (COM1, visible with -serial stdio)
 * and halt.
 */
#include "limine.h"
#include "lib/uart.h"

/* Limine v12 finds requests by scanning the kernel binary between the
 * start/end markers, so all three live in the same section, in order. */
#define LIMINE_REQ __attribute__((section(".limine_requests"), used))

static volatile uint64_t requests_start[4] LIMINE_REQ = LIMINE_REQUESTS_START_MARKER;

static volatile struct limine_bootloader_info_request bootloader_info_request LIMINE_REQ = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID,
    .revision = 0,
    .response = 0,
};

static volatile uint64_t requests_end[4] LIMINE_REQ = LIMINE_REQUESTS_END_MARKER;

void _start(void)
{
    uart_init();
    uart_puts("MirixOS phase 0: kernel entered (64-bit, higher half)\n");

    if (bootloader_info_request.response != 0) {
        uart_puts("bootloader: ");
        uart_puts(bootloader_info_request.response->name);
        uart_puts(" ");
        uart_puts(bootloader_info_request.response->version);
        uart_puts("\n");
    } else {
        uart_puts("bootloader info request: NO RESPONSE\n");
    }

    for (;;)
        __asm__ volatile ("hlt");
}
