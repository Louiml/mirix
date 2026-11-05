/* MirixOS kernel entry — Phase 2a: GDT/TSS/IDT/exceptions + panic screen.
 *
 * Boot order: UART -> Limine requests -> framebuffer console -> GDT/TSS ->
 * IDT (+PIC remap). After that, any CPU exception lands in panic_dump_regs
 * and shows a red screen with a register dump.
 *
 * Boot with `cmdline: crash` (see tools/test_crash.py) to deliberately
 * dereference NULL and watch the #PF panic.
 */
#include "limine.h"
#include "lib/uart.h"
#include "lib/string.h"
#include "lib/panic.h"
#include "drivers/fb.h"
#include "drivers/pic.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/idt.h"
#include "arch/x86_64/cpu.h"

#define LIMINE_REQ __attribute__((section(".limine_requests"), used))

static volatile uint64_t requests_start[4] LIMINE_REQ = LIMINE_REQUESTS_START_MARKER;

static volatile struct limine_bootloader_info_request bootloader_info_request LIMINE_REQ = {
    .id = LIMINE_BOOTLOADER_INFO_REQUEST_ID, .revision = 0, .response = 0,
};
static volatile struct limine_executable_cmdline_request cmdline_request LIMINE_REQ = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID, .revision = 0, .response = 0,
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
    uart_printf("MirixOS phase 2a: kernel entered (64-bit, higher half)\n");

    if (bootloader_info_request.response != 0)
        uart_printf("bootloader: %s %s\n",
                    bootloader_info_request.response->name,
                    bootloader_info_request.response->version);

    if (executable_address_request.response != 0)
        uart_printf("kernel image: physical_base=%p virtual_base=%p\n",
                    (void *)executable_address_request.response->physical_base,
                    (void *)executable_address_request.response->virtual_base);

    if (hhdm_request.response != 0) {
        g_hhdm_offset = hhdm_request.response->offset;
        uart_printf("HHDM offset: %p\n", (void *)g_hhdm_offset);
    } else {
        kpanic("no HHDM response from Limine\n");
    }

    uint64_t usable = 0;
    if (memmap_request.response != 0) {
        struct limine_memmap_response *mm = memmap_request.response;
        uart_printf("memmap: %llu entries\n", (unsigned long long)mm->entry_count);
        for (uint64_t i = 0; i < mm->entry_count; i++) {
            struct limine_memmap_entry *e = mm->entries[i];
            uart_printf("  [%llu] %p len=%llx %s\n", (unsigned long long)i,
                        (void *)e->base, (unsigned long long)e->length,
                        memmap_type_name(e->type));
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
        fb_clear(0x0d0d18);
        fb_text(40, 40, 4, 0xffffff, "Hello from MirixOS!");
        fb_text(40, 40 + 8 * 4 + 16, 2, 0x8899aa, "Phase 2a: GDT + IDT + exceptions");
        static const uint32_t bars[] = {
            0xff0000, 0xff8800, 0xffff00, 0x00ff00,
            0x00ffff, 0x0088ff, 0x8800ff, 0xffffff,
        };
        for (int i = 0; i < 8; i++)
            for (uint32_t y = 200; y < 224; y++)
                for (uint32_t x = 40 + (uint32_t)i * 60; x < 40 + (uint32_t)i * 60 + 56; x++)
                    fb_putpixel(x, y, bars[i]);
    } else {
        uart_printf("framebuffer request: NO RESPONSE\n");
    }

    /* CPU core: GDT/TSS, IDT, PIC */
    gdt_init();
    idt_init();
    pic_init();
    uart_printf("cpu core: gdt/tss/idt/pic ready\n");
    if (fb_ready()) {
        fb_puts("\ncpu core: gdt/tss/idt/pic ready\n");
        fb_puts("exceptions -> red panic screen\n");
    }

    /* Optional deliberate crash for the automated test: cmdline "crash" */
    const char *cmdline = (cmdline_request.response != 0)
                              ? cmdline_request.response->cmdline : "";
    uart_printf("cmdline: '%s'\n", cmdline);
    if (strncmp(cmdline, "crash", 5) == 0) {
        uart_printf("crash-test: dereferencing NULL -> expect #PF panic\n");
        if (fb_ready())
            fb_puts("crash-test: dereferencing NULL...\n");
        *(volatile uint64_t *)0 = 0xdeadbeef;   /* -> #PF, vector 14 */
        kpanic("crash-test failed: NULL write did not fault?!\n");
    }

    uart_printf("halting.\n");
    for (;;)
        hlt();
}
