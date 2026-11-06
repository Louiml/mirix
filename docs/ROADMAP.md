# MirixOS Roadmap

Strictly one phase at a time; each phase ends with something bootable/verifiable.

- [x] **Phase 0: Environment & repo** — WSL toolchain, repo layout, Makefile +
      `tools/mkiso.py`, Limine v12.9.6 pinned, `make test-boot` PASS (headless
      QEMU boots the ISO, kernel prints its banner on serial).
- [x] **Phase 1: Boot** — Limine requests (framebuffer, HHDM, memmap, kernel
      address), framebuffer console (font8x8, public domain) + serial console,
      "Hello from MirixOS!" banner + color bars on screen. `make test-boot`
      PASS: serial banner + QEMU screendump verified (1280x720, text rendered).
- [ ] **Phase 2: Kernel core**
  - [x] **2a** — GDT/TSS/IDT (256 gates), all 32 exceptions → red panic screen
        with register dump + CR2 (`make test-crash` PASS: #PF, cr2=0x0,
        97% red screendump), PIC remapped to 0x20+ and masked.
  - [x] **2b** — PMM (bitmap over Limine memmap, 117536/130668 pages free),
        VMM map/unmap/get_physical on Limine's PML4 (roundtrip PASS),
        `kmalloc`/`kfree` first-fit heap at `0xffffffffc0000000` (self-test
        PASS: 64 blocks alloc/verify/free/realloc). `make test-boot` PASS.
  - [x] **2c** — timer: TSC calibrated via PIT ch0 one-shot (measured 2.72 GHz),
        LAPIC bus calibrated via one-shot vs TSC (QEMU TCG ~929 MHz), periodic
        LVT timer on vector 0x30 at 100 Hz. Boot prints `timer: 20 ticks in
        200087 us window` (exactly 100 Hz). `make test-boot` PASS.
- [x] **Phase 3: Processes & syscalls**
  - [x] **3a** — kernel threads + round-robin scheduler, preempted by the
        100 Hz timer IRQ (frame-rewrite switch in the IRQ handler).
        `make test-boot` PASS: interleaved `[alpha]`/`[beta]`/`[main]`
        output, `sched: 15 switches, 81 ticks`.
  - [x] **3b** — `syscall`/`sysret` (EFER.SCE, STAR/LSTAR/SFMASK), ring 3 via
        TSS rsp0 + iretq to cs=0x23/ss=0x1B, ELF64 loader mapping PT_LOAD at
        0x400000 with PTE_USER (embedded hello.elf), syscalls write/exit/
        gettid. `make test-boot` PASS: `hello from ring 3!`, `ret=00000024`,
        `tid=0`, `[user] exit(42)`.
  - [x] **3c** — processes with per-process address spaces: `process_spawn_user`
        gives each process its own PML4 (kernel half copied from boot PML4),
        kernel stack (TSS rsp0 + syscall-entry stack, swapped atomically with
        CR3 in the timer-IRQ frame-rewrite switch), fabricated ring-3 resume
        frame; preemption in ring 3 enabled; new `SYS_yield`; `SYS_exit` parks
        the process DEAD instead of halting. Syscall user context kept on the
        per-process kernel stack (not globals — see DESIGN.md). Two spawned
        user processes interleave `P3.x`/`P4.x` output and exit(43)/exit(44);
        `make test-boot` + `make test-crash` PASS.
- [ ] **Phase 4: Devices & storage**
  - [x] **4a** — PCI enumeration via ECAM/MMCONFIG (on this OVMF+q35 the ECAM
        hole is the 256 MiB reserved memmap entry at **0xE0000000**, not the
        classic 0xB0000000 — reading the memmap beats hardcoding; first 1 MiB
        of bus 0 mapped into kernel space at 0xffffffffe0000000). Found 6
        devices: host bridge 8086:29c0, VGA 1234:1111, NIC 8086:10d3
        (class 02), LPC 8086:2918, **AHCI 8086:2922 (class 01.06.01)**, SMBus.
        PS/2 keyboard on IRQ1 (i8042 init, translation bit → scancode set 1,
        make/break + shift tracking, 256-entry event ring buffer; consumer
        polls kbd_pop). Verified by injecting `sendkey h/i/spc/ret` through
        the QEMU monitor: serial shows `[kbd] 'h'`/`'i'`/`SPACE`/`ENTER`.
        `make test-boot` + `make test-crash` PASS.
  - [ ] **4b** — VFS + ramdisk (syscalls open/read/write), then AHCI disk.
- [ ] **Phase 5: Userland foundation** — minimal libc, `init`, `mirsh`,
      `ls`/`cat`/`echo`/`mkdir`/`ps`, pipes, TTY layer.
- [ ] **Phase 6: Graphics stack** — framebuffer driver, double buffering,
      TrueType rendering, input event queue, cursor.
- [ ] **Phase 7: Aurora desktop** — window server + compositor (alpha,
      rounded corners, shadows, blur), menu bar, Dock with magnification,
      WM (drag/resize/min/max, traffic lights), C++ widget toolkit, apps:
      Terminal, Navigator (file manager), Text Editor, System Settings,
      Quick Find (launcher).
- [ ] **Phase 8: Polish & extras** — virtio-net + TCP/IP, SMP, power
      management, optional Rust modules, installer ISO, VirtualBox/real
      hardware testing.

## Definition of done (v1.0)

Boots to the graphical desktop in QEMU, runs multiple user-mode GUI apps at
once, reads/writes its filesystem, takes keyboard+mouse input, and survives a
30-minute stress run without a kernel panic.
