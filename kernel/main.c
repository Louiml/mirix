/* MirixOS kernel entry — Phase 1: boot + framebuffer + serial console.
 *
 * Limine drops us here in 64-bit long mode with paging on, a stack, the
 * kernel mapped in the higher half, and a higher-half direct map (HHDM) of
 * all physical RAM. We ask Limine for what we need via "requests": the
 * marker-delimited list below is scanned by Limine in the kernel image, and
 * it fills in each request's `response` pointer before jumping to _start.
 */
#include "limine.h"
#include "lib/uart.h"
#include "drivers/fb.h"

/* All requests live in one section, bracketed by the start/end markers,
 * in declaration order — Limine scans for the markers in the image. */
#define LIMINE_REQ __attribute__((section(".limine_requests"), used))

static volatile uint64_t requests_start[4] LIMINE_REQ = LIMINE_REQUESTS_START_MARKER;

static volatile struct limine_bootloader_info_request bootloader_info_request LIMINE_REQ = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = 0,
};
static volatile struct limine_executable_address_request executable_address_request LIMINE_REQ = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID, .revision = 0, .response = 0,
};
static volatile struct limine_hhdm_request hhdm_request LIMINE_REQ = {
    .id = LIMINE_HHDM_REQUEST_ID, .revision = 0, .response = 0,
};
static volatile struct limine_memmap_request memmap_request LIMINE_REQ = {
    .id = LIMINE_MEMMAP_REQUEST_ID, .revision = 0, .response = 0,
};
static volatile struct limine_framebuffer_request framebuffer_request LIMINE_REQ = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID, .revision = 0, .response = 0,
};

static volatile uint64_t requests_end[4] LIMINE_REQ = LIMINE_REQUESTS_END_MARKER;

/* The HHDM offset: physical address P is accessible at virtual HHDM+P.
 * Phase 2's memory manager will build on this. */
uint64_t g_hhdm_offset;

static const char *memmap_type_name(uint64_t t)
{
    switch (t) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi-reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi-nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader-reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel+modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved-mapped";
    default:                                   return "?";
    }
}

void _start(void)
{
    uart_init();
    uart_printf("MirixOS phase 1: kernel entered (64-bit, higher half)\n");

    if (bootloader_info_request.response != 0)
        uart_printf("bootloader: %s %s\n",
                    bootloader_info_request.response->name,
                    bootloader_info_request.response->version);
    else
        uart_printf("bootloader info request: NO RESPONSE\n");

    if (executable_address_request.response != 0)
        uart_printf("kernel image: physical_base=%p virtual_base=%p\n",
                    (void *)executable_address_request.response->physical_base,
                    (void *)executable_address_request.response->virtual_base);

    if (hhdm_request.response != 0) {
        g_hhdm_offset = hhdm_request.response->offset;
        uart_printf("HHDM offset: %p (physical P -> virtual %p)\n",
                    (void *)g_hhdm_offset, (void *)g_hhdm_offset);
    }

    if (memmap_request.response != 0) {
        struct limine_memmap_response *mm = memmap_request.response;
        uint64_t usable = 0;
        uart_printf("memmap: %llu entries\n", (unsigned long long)mm->entry_count);
        for (uint64_t i = 0; i < mm->entry_count; i++) {
            struct limine_memmap_entry *e = mm->entries[i];
            uart_printf("  [%llu] %p len=%llx %s\n",
                        (unsigned long long)i, (void *)e->base,
                        (unsigned long long)e->length, memmap_type_name(e->type));
            if (e->type == LIMINE_MEMMAP_USABLE)
                usable += e->length;
        }
        uart_printf("usable RAM: %llu MiB\n", (unsigned long long)(usable >> 20));
    }

    /* Framebuffer console */
    if (framebuffer_request.response != 0 &&
        framebuffer_request.response->framebuffer_count > 0) {
        struct limine_framebuffer *fb = framebuffer_request.response->framebuffers[0];
        uart_printf("framebuffer: %llux%llu bpp=%u pitch=%llu addr=%p\n",
                    (unsigned long long)fb->width, (unsigned long long)fb->height,
                    fb->bpp, (unsigned long long)fb->pitch, fb->address);
        fb_init(fb);
        fb_clear(0x0d0d18);   /* dark blue-black background */

        /* Big banner (scale 4) so a screendump clearly shows text. */
        fb_text(40, 40, 4, 0xffffff, "Hello from MirixOS!");
        fb_text(40, 40 + 8 * 4 + 16, 2, 0x8899aa,
                "Phase 1: framebuffer + serial console");

        /* Color bars to prove channel packing works. */
        static const uint32_t bars[] = {
            0xff0000, 0xff8800, 0xffff00, 0x00ff00,
            0x00ffff, 0x0088ff, 0x8800ff, 0xffffff,
        };
        for (int i = 0; i < 8; i++)
            for (uint32_t y = 200; y < 224; y++)
                for (uint32_t x = 40 + (uint32_t)i * 60; x < 40 + (uint32_t)i * 60 + 56; x++)
                    fb_putpixel(x, y, bars[i]);

        fb_puts("\nMirixOS console ready.\n");
        fb_printf("fb console: %ux%u, %u bpp\n", fb_width(), fb_height(), 32);
        uart_printf("screen: banner + color bars drawn\n");
    } else {
        uart_printf("framebuffer request: NO RESPONSE\n");
    }

    uart_printf("halting.\n");
    for (;;)
        __asm__ volatile ("hlt");
}
