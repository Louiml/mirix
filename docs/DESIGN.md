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

## CPU core (Phase 2a, done)

- GDT: null, kcode `0x08`, kdata `0x10`, ucode `0x18`, udata `0x20`, TSS `0x28`
  (flat, limit 4 GiB). Loaded via `lgdt` + far return (`gdt.asm`); TSS via `ltr`.
- TSS: `rsp0` = static 16 KiB kernel stack; IST1 = 4 KiB stack for #DF.
- IDT: 256 interrupt gates (0x8E), exceptions 0–31 from `isr.asm` stubs
  (error-code vectors 8,10–14,17,21,29,30 push the real code, others a dummy),
  IRQs 32–63. `lidt` in `idt_init`.
- Exception path: `isr_handler` → `panic_dump_regs` → red fb screen + serial
  dump (vector, error, rip/cs/rflags/rsp/ss, cr2/cr3, all GPRs) → halt.
  Verified by `make test-crash` (cmdline `crash` → NULL store → #PF).
- PIC 8259 remapped to 0x20–0x2f, all masked. LAPIC EOI via HHDM+0xFEE000B0.

## Memory management (Phase 2b, done)

- HHDM: Limine maps all physical RAM at `g_hhdm_offset` (0xffff800000000000);
  `phys_to_virt()` in `mm/mm.h` converts any physical address.
- PMM (`kernel/mm/pmm.c`): bitmap, 1 bit per 4 KiB page, sized to the highest
  `usable` memmap address. Bitmap itself lives in the first usable region that
  fits (observed: phys 0x1000). Init: mark all used → free usable regions →
  re-mark bitmap pages + page 0. Alloc = word-skip scan (skip full 0xff qwords)
  for first 0 bit; free clears the bit. 117536/130668 pages free on a 512 MiB VM.
- VMM (`kernel/mm/vmm.c`): walks the current PML4 (read CR3, access via HHDM);
  `walk_alloc` allocates+zeroes missing PDPT/PD/PT levels from the PMM and
  links them with PRESENT|RW (+USER if requested). `vmm_map/unmap/get_physical`,
  `invlpg` after changes, 2 MiB huge-page aware in get_physical.
- Heap (`kernel/mm/kmalloc.c`): first-fit free-list of `block_t{size,free,next}`
  headers, address-ordered, over a virtual range at `KHEAP_VIRT=0xffffffffc0000000`
  (grown on demand by mapping fresh PMM pages, 32 MiB cap). Split on alloc when
  the remainder fits header+16 B; forward-coalesce on free. Self-test:
  64 pseudo-random blocks, pattern verify, free even-indexed, realloc, verify
  odd blocks untouched → PASS.

## Timer (Phase 2c, done)

- TSC calibration (`drivers/timer.c`): PIT channel 0 one-shot, divisor 65536
  (~54.9 ms at 1.193182 MHz). Two-phase latch poll — first wait for the count
  to drop below 0xf000 (stale 0xffff reads after reload would otherwise end
  the window instantly), then until 0 / wrap. `tsc_hz = dTSC * 1193182 / 65536`.
  Measured under QEMU TCG: ~2.72 GHz.
- LAPIC (`drivers/lapic.c`): xAPIC at phys 0xFEE00000 via HHDM, enabled through
  MSR 0x1B (global enable + xAPIC mode), SVR = 0xff | enable. Registers:
  EOI 0xB0, SVR 0xF0, timer divide 0x3E0, init count 0x380, LVT 0x320.
- LAPIC bus calibration: one-shot LVT timer (divide 1) with 50M counts,
  measured against the calibrated TSC → bus Hz (QEMU TCG assumes ~1 GHz bus,
  KVM uses the guest TSC — calibrating handles both). Then periodic mode,
  vector 0x30 (48, above the 16 PIC IRQs), count = bus_hz/100 → 100 Hz.
- `timer_irq()` (idt.c routes vector 0x30): `ticks++` + LAPIC EOI. Verified:
  20 ticks in a 200.087 ms TSC-measured window.
- `msleep()` spins on rdtsc with `pause` (not hlt) so the wait never depends
  on an IRQ arriving.

## Threads & scheduler (Phase 3a, done)

- `kernel/proc/thread.c`: fixed array of `thread_t` (id, name, state,
  `registers_t saved`, stack from `kmalloc` 16 KiB, entry fn). `sched_init()`
  adopts the boot stack as thread 0 "main".
- **Context switch without asm**: the timer IRQ stub already pushed a full
  `registers_t` frame. `sched_switch_from_irq(frame)` copies the live frame
  into `current->saved` (forcing `rflags |= 0x200` — interrupt gates clear IF,
  so the saved frame would otherwise resume with interrupts off), picks the
  next READY thread round-robin, and copies its `saved` frame over the live
  one. The normal `irq_common` epilogue + `iretq` then pops the *next*
  thread's GPRs and rip/cs/rflags/rsp/ss — the CPU becomes that thread.
  Atomic because the handler runs with IF=0.
- New threads get a fabricated frame: rip = `thread_trampoline`,
  cs=0x08, ss=0x10, rflags=0x202, rsp = top of their stack. The trampoline
  calls the entry fn; on return the thread marks itself DEAD and halts.
- Quantum: every 4 ticks (40 ms) `sched_tick()` switches. Verified:
  interleaved `[alpha]`/`[beta]`/`[main]` serial output, 15 switches.

## Syscalls & ring 3 (Phase 3b, done)

- GDT order matters for SYSRET: SS = STAR[63:48]+8, CS = STAR[63:48]+16, so
  user segments are `udata=0x18`, `ucode=0x20` (data before code) and STAR's
  user base is 0x10. Kernel CS=0x08/SS=0x10 via STAR[47:32].
- MSRs: EFER.SCE (0xC0000080 bit 0), STAR=0xC0000081, LSTAR=0xC0000082 =
  `syscall_entry`, SFMASK=0xC0000084 = 0x200 (IF cleared on entry so the
  stack swap is safe).
- `syscall.asm`: entry saves user rsp/rip/rflags, switches to a dedicated
  syscall stack, **saves/restores all argument GPRs around the C call**
  (SYSCALL only guarantees rcx/r11; the kernel preserves the rest,
  Linux-style — forgetting this silently corrupts user rdi across calls),
  shuffles args to SysV order, calls `syscall_dispatch`, then `o64 sysret`.
- ABI: rax=nr, rdi/rsi/rdx/r10/r8/r9 = args, rax = return.
- Ring 3 entry: `enter_usermode` pushes ss=0x1B, rsp, rflags=0x202,
  cs=0x23, rip and `iretq`s. TSS.rsp0 = separate ring-0 IRQ stack.
- ELF loader (`proc/elf.c`): validates ELF64/ET_EXEC/EM_X86_64, two passes —
  map all PT_LOAD ranges (+ user stack at 0x800000) once, then memcpy file
  bytes. Limine identity-maps the low half without USER, so existing
  identity pages (phys==va) are unmapped and remapped with PTE_USER;
  walk_alloc ORs USER into pre-existing upper-level entries (user access
  needs U=1 at EVERY level). User pages are mapped into the kernel's
  address space for now (per-process PML4 = Phase 3c).
- Syscalls: 0=write(fd,buf,len) (fd 1/2 = serial, user-pointer validated),
  1=exit(code), 2=gettid. Scheduler does not preempt while in ring 3
  (`sched_set_user_mode`) — proper user preemption needs per-process
  address spaces (3c).
- User program: `user/hello.c` + `start.asm`, freestanding, linked at
  0x400000 (`user/hello.ld`), embedded via `tools/elf2h.py` ->
  `build/hello_elf.h`.

## Processes & address spaces (Phase 3c, done)

- `thread_t` now carries `pml4_phys`, `kstack_top`, `is_user`. Kernel threads
  run ring 0; user processes get a fabricated ring-3 frame (cs=0x23, ss=0x1B,
  rsp=USER_STACK_TOP) resumed through the same iretq path as kernel threads.
- `vmm_create_space()` allocates a fresh PML4 and copies entries 256-511
  (kernel half: HHDM, kernel image, heap) from the current one; the user half
  is private. `process_spawn_user()` creates the space, switches CR3 to it,
  runs the ELF loader, switches back.
- The timer-IRQ frame-rewrite switch now also swaps `write_cr3(next->pml4)`,
  `tss_set_rsp0(next->kstack_top)` and `g_current_kstack_top` inside the same
  IF=0 critical section, so a ring-3 process preempted by the timer resumes
  under its own CR3 with its own kernel stack. Preemption in ring 3 is on.
- `syscall_entry` switches to `g_current_kstack_top` (per-process). The user
  rip/rflags/rsp are saved **on that kernel stack**, not in globals:
  `sched_yield()`/`proc_exit_current()` do `sti; hlt` mid-syscall, so another
  process can enter a syscall while this one is parked — with globals the parked
  process would `sysret` to the other process's rip under its own CR3 (observed
  as #GP at the ring-3 `hlt` right after `exit`). `g_scratch` is only touched
  with IF=0, before the stack swap, so it is race-free.
- `SYS_yield` (3) = cooperative reschedule: force_switch + sti/hlt, the timer
  IRQ switches away and the saved frame resumes right after the hlt.
  `SYS_exit` prints `[user <tid>] exit(<code>)`, marks the process DEAD and
  parks in a hlt loop; the scheduler only reselects TH_READY, so DEAD
  processes are never resumed (reaping = never reselecting; no free yet).
- Verified: two spawned processes (same embedded ELF, separate PML4s)
  interleave `P3.0..P3.4` / `P4.0..P4.4` on serial, then `[user 3] exit(43)`,
  `[user 4] exit(44)`; main keeps idling with the switch counter climbing.

## Syscall table

Interface via `syscall`/`sysret`, number in `rax`, args in
`rdi, rsi, rdx, r10, r8, r9`, return in `rax`:

| nr | name | args | notes |
|---|---|---|---|
| 0 | `write` | fd, buf, len | fd 1/2 = serial, else VFS file; user ptrs validated |
| 1 | `exit` | code | marks process DEAD, parks (never returns) |
| 2 | `gettid` | — | current process id |
| 3 | `yield` | — | cooperative reschedule via timer IRQ |
| 4 | `open` | path, flags | O_CREAT 0x40, O_TRUNC 0x80; `"/"` = root dir; returns fd |
| 5 | `read` | fd, buf, len | fd 0 = keyboard (non-blocking), else VFS file at fd offset |
| 6 | `close` | fd | frees the fd slot (file stays in ramdisk) |
| 7 | `getdents` | fd, buf, len | dir only; fills 40-byte `mirix_dirent_t[]`, returns bytes |
| 8 | `ps` | buf, len | fills 32-byte `ps_entry_t[]` (id, name[24], state) |
| 9 | `diskread` | lba, nsec, buf | AHCI read (≤64 sectors) via contiguous PMM bounce, copied to user |

## VFS + ramdisk (Phase 4b, done)

- `kernel/fs/vfs.c`: vnode table `g_vnodes[34]` — 0 console, 1 root dir, 2..33
  files. Flat namespace (`/hello.txt` → `hello.txt`). File data in a
  kmalloc'd buffer, `cap` doubles on write (alloc+memcpy+kfree), capped at
  256 KiB. All VFS calls run in syscall context with IF=0 on one CPU →
  atomic, no locks.
- Per-process fd table in `thread_t` (`fds[16]`, 0/1/2 = console vnode 0,
  each with its own `off` cursor). Accessors `proc_fd_alloc/close/vnode/offset`
  operate on `g_current` (correct: syscalls run on the caller's kstack).
- Syscall dispatch copies paths out of user memory via `user_strcpy`
  (range-checked against the kernel half; dispatch runs under the caller's
  CR3 so user pages are readable directly).
- `mirix_dirent_t` is a fixed 40-byte struct shared with userland
  (`name[32], size u32, type u8, pad[3]`).
- Userland builds each output line in a buffer and emits it with ONE `write`
  so timer preemption between syscalls can't interleave half-lines.

## Framebuffer TTY / serial mirror (Phase 5b, done)

- `uart_putc` ends with `if (fb_ready() && fb_mirror()) fb_putc(c);` — every
  serial byte also renders on the scrolling fb console (no recursion:
  `fb_putc` never calls uart). `g_mirror` defaults to 1 in `fb_init`;
  `kpanic`/`panic_dump_regs` call `fb_set_mirror(0)` first so the red panic
  screen isn't polluted by further log output.
- `FB_HEADER_PX` (16 px) top strip with 8 color bars (`fb_draw_header`),
  drawn once after `fb_clear` in main. `fb_scroll` now scrolls ONLY the text
  area below the strip (memmove of `fb + header*pitch` region) — the old code
  `return`ed when the cursor hit the bottom and never scrolled. Cursor home
  is `(0, FB_HEADER_PX)` in both `fb_init` and `fb_clear`.
- Kernel log atomicity: lines that tests grep as a unit (spawn/create,
  `sched:` summary) are printed with IF=0; user output is atomic via
  single-write lines.

## AHCI disk (Phase 4c, done)

- `ahci.c` maps ABAR (BAR5) into kernel space and drives port 0 with polling
  (IE=0). All DMA structures live in ONE contiguous PMM block
  (`pmm_alloc_pages_contig(24)` = 96 KiB): CL at +0, FIS RX at +0x400, command
  tables at +0x1000, data bounce at +0x2000. Port reg struct is exactly 0x80
  bytes (reserved word at 0x1c included — an early off-by-one-register here
  made PxSSTS read PxSIG).
- QEMU quirk: its AHCI uses bit0 of GHC as HR (reset, self-clearing; leaves
  bit31 set) and bit31 as AE — opposite of the spec. Writing GHC=1 therefore
  resets and leaves the controller enabled.
- Reads: H2D FIS (0x27, C=1, cmd 0x25 READ DMA EXT, 48-bit LBA), one PRDT
  entry (DBC = bytes-1), PxCI=1, poll PxCI clear + !TFES + !(BSY|DRQ).
- `SYS_diskread(lba, nsec, buf)` allocates `ceil(nsec*512/4096)` contiguous
  pages, reads into them via HHDM, memcpy's to the user buffer, frees.

## PCI & PS/2 input (Phase 4a, done)

- **PCI:** config space via ECAM/MMCONFIG. The base is NOT assumed: on this
  OVMF+q35 boot the memmap's 256 MiB reserved hole at `0xE0000000` is the ECAM
  (OVMF relocates MMCONFIG; `0xB0000000` reads as zeros). `pci.c` maps the
  first 1 MiB (bus 0) to `0xffffffffe0000000` with `vmm_map` and scans
  dev 0-31 x func 0-7 (header-type bit 7 decides multifunction). Devices go
  into a 64-entry table; `pci_find_by_class(0x01, 0x06)` will find the AHCI
  controller in 4b.
- **PS/2 keyboard:** i8042 at 0x60/0x64. Init: disable ports, flush, set
  config bits (IRQ1 enable 0x01, translation 0x40 so the controller emits
  **scancode set 1**, kbd clock on), re-enable port, device reset (0xFF →
  ACK 0xFA → BAT 0xAA), defaults (0xF6), enable scanning (0xF4), then
  `pic_clear_mask(1)`. All waits are bounded loops (never hang the boot).
- IRQ1 (vector 0x21) is routed in `idt.c` to `ps2_keyboard_irq()`, which
  reads 0x60, decodes make/break (`bit7`), 0xE0 extended prefix, tracks
  shift (0x2A/0x36), maps to ASCII via lower/shift tables, and pushes
  `kbd_event_t` onto a 256-entry ring buffer. Single producer (IRQ, IF=0) /
  single consumer (main loop polling `kbd_pop`) — no locking needed.
- Headless testing trick: QEMU monitor `sendkey h` injects into the emulated
  PS/2 keyboard even with `-display none`; `tools/test_boot.py` sends
  `h i spc ret` and greps the serial for the `[kbd]` echo lines.

## Graphics stack (Phase 6, done)

- **6a double buffering:** `fb_enable_double_buffer()` (after heap init)
  allocates a back buffer; every draw op writes `g_draw` (back buffer when
  present) and sets `g_dirty`. `fb_present()` memcpy's back→front when dirty;
  the main loop calls it every 20 ms (~50 Hz) and both panic paths call it
  once (so the red screen still appears — crash test 97.4% red).
  `SYS_flip(10)` lets userland request a present; `SYS_fbinfo(11)` returns
  (w, h, 32).
- **6b mouse:** IRQ12 (vector 0x2c) → `ps2_mouse_irq()`. The i8042 aux port
  is enabled (0xA8, config bit 1 = IRQ12, 0xF4 reporting, 100 Hz sample
  rate). Packets are 3 bytes; X/Y are 9-bit two's complement (sign-extend
  from bits 4/5 of byte 0). Position is accumulated absolutely, clamped to
  the screen, and the arrow is composited onto the FRONT buffer inside
  `fb_present()` — never into the back buffer, and `fb_set_cursor` marks
  `g_dirty` so a moved cursor erases its old pixels. IRQ2 (cascade) must be
  unmasked on the master PIC. `SYS_getevent(12)` pops a unified
  `input_event_t` (kbd or mouse); `SYS_mousepos(13)` returns x/y/buttons.
- **6c TrueType (`kernel/lib/ttf.c`):** sfnt tables are BIG-endian (an early
  little-endian reader silently failed every check). cmap format 4 binary
  search; loca short/long per `indexToLocFormat`; simple glyphs (flags with
  repeat runs, delta-encoded coords) and composite glyphs (2.14 affine
  transforms composed recursively). Outlines are flattened in device space
  (y flipped), quadratic curves sampled 16x with int64 De Casteljau, then a
  nonzero-winding scanline fill at 4x horizontal subsampling produces an
  8-bit alpha bitmap per glyph, cached in a 192-entry LRU keyed
  (codepoint, px). `ttf_draw_text` blends via `fb_blendpixel` (src-over) at
  baseline = y + ascender*px/upm. Scale factor S = px*65536/upm, so
  device16 = font_units * S (the <<16 lives inside S). Font: DejaVuSans.ttf
  embedded as a C array (`build/DejaVuSans_ttf.h`), OFL license in
  `assets/fonts/`.

## Window manager / compositor (Phase 7a, done)

- Windows are kernel objects (`wm_win_t`, 16 max). Each owns
  `WIN_BUF_PAGES` (1024) physically contiguous pages = one 4 MiB 32-bpp
  buffer (`pmm_alloc_pages_contig`). `SYS_win_create` maps it into the
  caller's address space at `WIN_BUF_VA_BASE + wid*0x400000`
  (`vmm_map_in_space(pml4, va, phys, P|RW|USER)` — walks the target PML4
  via the HHDM, no CR3 switch).
- The owner paints its buffer directly (0xRRGGBB pixels, blitted by the
  compositor with `fb_putpixel`), then `SYS_win_commit(wid)` sets the
  compositor dirty flag. `wm_render()` (called from the main loop when
  `wm_dirty() || fb_dirty()`) draws: cached background (vertical gradient
  + 3 translucent diagonal aurora bands, precomputed once), a menu bar
  (dark strip + TTF "Aurora"/"MirixOS"), then windows back-to-front in a
  z-list: two-pass alpha drop shadow (`fill_round` + `fb_blendpixel`),
  rounded body + title bar, three traffic-light circles (red/yellow/green),
  centered 13 px TrueType title, the content blit honoring rounded corners,
  and a blue focus ring on the focused window.
- Syscalls 14–21: `win_create(x,y,w,h,title,flags)`, `win_commit`,
  `win_move`, `win_resize`, `win_close`, `win_focus` (raise+focus),
  `win_top(x,y)` (hit-test, topmost first), `uptime` (ticks*1000/HZ).
  `WM_FRAMELESS` windows (the Dock) get no title bar.
- Input split: `input_pop_filtered(out, filter)` — filter 1 = mouse only,
  2 = keyboard only. Aurora pops mouse-only events (and `SYS_mousepos` for
  the absolute position, so it does not depend on the ring at all); the
  shell keeps reading fd 0 (keyboard) via `kbd_pop`.
- QEMU monitor `mouse_move dx dy` is RELATIVE, but QEMU keeps an internal
  absolute cursor clamped to the screen when computing the deltas it
  injects — so a move that would push the cursor off-screen is silently
  dropped. Tests must use in-bounds relative moves (start at 640,360).

## Driver list

| Device | Driver | Phase |
|---|---|---|
| Serial UART 16550 (debug) | `kernel/lib/uart.c` | 0 (done) |
| Framebuffer console | `kernel/drivers/fb.c` | 1 |
| APIC/HPET timer | `kernel/drivers/timer.c` | 2 |
| PS/2 keyboard/mouse | `kernel/drivers/ps2.c` | 4a/6b (done) |
| PCI enumeration | `kernel/drivers/pci.c` | 4a (done) |
| VFS + ramdisk | `kernel/fs/vfs.c` | 4b (done) |
| AHCI SATA disk | `kernel/drivers/ahci.c` | 4c (done) |
| Double-buffered fb + cursor | `kernel/drivers/fb.c` | 6a/6b (done) |
| TrueType renderer | `kernel/lib/ttf.c` | 6c (done) |
| Window manager / compositor | `kernel/gui/wm.c` | 7a (done) |
| Aurora desktop process | `user/aurora.c` | 7a (done) |
| Framebuffer TTY (serial mirror) | `kernel/drivers/fb.c`, `lib/uart.c` | 5b (done) |
| mirsh shell | `user/shell.c` | 5a (done) |
| virtio-blk | `kernel/drivers/virtio_blk.c` | 4 |
| virtio-gpu | `kernel/drivers/virtio_gpu.c` | 6 |
| virtio-net | `kernel/drivers/virtio_net.c` | 8 |

## Visual identity ("Aurora")

Original design inspired by macOS: translucent blurred menu bar, centered
Dock with hover magnification, rounded windows with soft shadows and
red/yellow/green controls, alpha compositing. Assets: original or OFL
licensed only (e.g. Inter font). No Apple logos, SF fonts, or Apple icons.
