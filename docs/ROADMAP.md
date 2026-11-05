# MirixOS Roadmap

Strictly one phase at a time; each phase ends with something bootable/verifiable.

- [x] **Phase 0: Environment & repo** — WSL toolchain, repo layout, Makefile +
      `tools/mkiso.py`, Limine v12.9.6 pinned, `make test-boot` PASS (headless
      QEMU boots the ISO, kernel prints its banner on serial).
- [x] **Phase 1: Boot** — Limine requests (framebuffer, HHDM, memmap, kernel
      address), framebuffer console (font8x8, public domain) + serial console,
      "Hello from MirixOS!" banner + color bars on screen. `make test-boot`
      PASS: serial banner + QEMU screendump verified (1280x720, text rendered).
- [ ] **Phase 2: Kernel core** — GDT/TSS/IDT, exceptions + panic screen with
      register dump, physical memory manager (bitmap/buddy), paging (VMM),
      kernel heap (`kmalloc`/`kfree`), APIC/HPET timer.
- [ ] **Phase 3: Processes & syscalls** — threads, context switch (asm),
      round-robin → priority scheduler, `syscall`/`sysret`, ring 3, ELF loader,
      first user program, `fork`/`exec`/`exit`/`wait`.
- [ ] **Phase 4: Devices & storage** — PCI, PS/2 + USB-HID keyboard/mouse,
      virtio-blk/AHCI, VFS, ramdisk, FAT32 rw, custom MirixFS.
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
