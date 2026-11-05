# MirixOS Design Notes

Living document — updated at every milestone.

## Architecture

- x86_64, long mode, 4-level paging, higher-half kernel at `0xffffffff80000000`
  (Limine loads the ELF there; `-mcmodel=kernel`).
- Bootloader: Limine (protocol requests scanned between markers in
  `.limine_requests`). Pinned release: see `boot/LIMINE_VERSION.txt`.
- Monolithic kernel to start; internal interfaces kept clean so subsystems
  can move to user space later.
- Ring 0 kernel (C17, freestanding clang) / ring 3 userland (C++, no
  exceptions/RTTI in kernel-adjacent code). Assembly (NASM) only for boot
  stubs, context switch, interrupt/syscall entry.

## Memory map (Phase 1, verified in QEMU)

| Region | Address | Owner |
|---|---|---|
| Kernel image (virtual) | `0xffffffff80000000` + (physical_base from Limine, e.g. `0x1dc5c000`) | Limine maps ELF here |
| Higher-half direct map (HHDM) | `0xffff800000000000` + physical | all physical RAM, rw (observed offset `0xffff800000000000`) |
| VGA framebuffer | `HHDM + 0x80000000`, 1280x720x32, pitch 5120 | linear FB (observed) |
| Kernel heap | TBD (Phase 2, inside HHDM-managed region) | `kmalloc`/`kfree` |
| User space | `0x0000000000400000` … `0x00007fffffffffff` | per-process page tables |

Limine v12 notes (verified against v12.9.6): requests are structs between
`LIMINE_REQUESTS_START/END_MARKER`s in `.limine_requests` (page-aligned, RW);
config uses `path: boot():/boot/kernel.elf`; ELF PHDRs must not share a 4K
page across different permissions.

## Boot flow

1. UEFI (OVMF) or BIOS (SeaBIOS) → El Torito/EFI app → Limine (`limine.sys` / `BOOTX64.EFI`).
2. Limine reads `/boot/limine.conf`, loads `/boot/kernel.elf`, sets up
   paging + HHDM, jumps to `_start` in 64-bit mode.
3. Kernel answers Limine requests, brings up UART console (Phase 0), then
   framebuffer console (Phase 1).

## Syscall table

TBD — Phase 3. Interface via `syscall`/`sysret`, number in `rax`, args in
`rdi, rsi, rdx, r10, r8, r9`.

## Driver list

| Device | Driver | Phase |
|---|---|---|
| Serial UART 16550 (debug) | `kernel/lib/uart.c` | 0 (done) |
| Framebuffer console | `kernel/drivers/fb.c` | 1 |
| APIC/HPET timer | `kernel/drivers/timer.c` | 2 |
| PS/2 keyboard/mouse | `kernel/drivers/ps2.c` | 4 |
| PCI enumeration | `kernel/drivers/pci.c` | 4 |
| virtio-blk | `kernel/drivers/virtio_blk.c` | 4 |
| virtio-gpu | `kernel/drivers/virtio_gpu.c` | 6 |
| virtio-net | `kernel/drivers/virtio_net.c` | 8 |

## Visual identity ("Aurora")

Original design inspired by macOS: translucent blurred menu bar, centered
Dock with hover magnification, rounded windows with soft shadows and
red/yellow/green controls, alpha compositing. Assets: original or OFL
licensed only (e.g. Inter font). No Apple logos, SF fonts, or Apple icons.
