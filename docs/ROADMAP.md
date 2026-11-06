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
  - [x] **4b** — VFS + ramdisk: flat root `/`, 32 file slots, files are
        kmalloc'd buffers grown by alloc+copy (256 KiB max). Vnodes: 0 =
        console (write→serial, read→kbd ring), 1 = `/`, 2+ = files. Per-process
        fd table (16 fds, 0/1/2 = console) in `thread_t`. New syscalls:
        `open(4) read(5) close(6) getdents(7)`; `write(0)` now routes fd 1/2
        to serial and other fds to the VFS; user pointers range-checked, paths
        copied via bounded `user_strcpy`. Kernel spawn/create log lines are
        printed with IF=0 so timer preemption can't split them. User demo
        (`user/hello.c`) does create/write/close, read-back, `ls /` — serial
        shows `fsdemo: wrote 16 bytes...`, `read back: mirixfs says hi`,
        `ls / (1 entries): hello.txt` from BOTH processes. test-boot +
        test-crash PASS.
  - [x] **4c** — AHCI disk read, VERIFIED end-to-end. `kernel/drivers/ahci.c`
        finds 8086:2922 (class 01.06.01), maps BAR5 (ABAR) at
        `0xffffffffe1000000`, pulses GHC reset and enables AE — note QEMU's
        AHCI swaps the spec's GHC bits (bit0=HR reset self-clearing, bit31=AE),
        so `GHC=1` resets and leaves AE set. Port 0 comes up with one 96 KiB
        physically-contiguous PMM block holding CL (1 KiB) + FIS (256 B) +
        command tables (4 KiB) + data bounce (92 KiB); `pmm_alloc_pages_contig`
        added for this. READ DMA EXT (0x25) is issued on slot 0 and polled.
        `SYS_diskread(9)` bounce-reads up to 64 sectors into user memory;
        mirsh has `diskread <lba>`. QEMU attaches `build/mirix.img`
        (`tools/mkdisk.py`, 64 MiB, magic at LBA 0 and LBA 100) via
        `-device ide-hd,bus=ide.0`. Boot self-test prints
        `ahci: LBA 0 read OK (4096 bytes), magic='MIRIXAHCI0...'` and
        `LBA 100 ... 'MIRIXLBA100...'`; test_boot.py also types `diskread 0`
        into mirsh via sendkey. test-boot + test-crash PASS.
- [x] **Phase 5: Userland foundation**
  - [x] **5a** — `mirsh` shell (ring 3): reads lines from fd 0 (keyboard ring),
        echoes, backspace, commands `help ls cat echo write ps exit`; new
        `SYS_ps(8)` fills 32-byte `ps_entry_t[]` (id/name/state) via
        `sched_ps()`. Second embedded user ELF via generalized Makefile
        pattern (`USER_PROGS := hello shell`). `tools/test_boot.py` drives the
        shell headless through the QEMU monitor `sendkey` (types `ls`, `ps`):
        serial shows `ls: 1 entries` / `hello.txt (16 bytes)` and the full
        `ps:` table with `name=shell state=RUNNING`. test-boot + test-crash
        PASS.
  - [x] **5b** — framebuffer TTY: `uart_putc` mirrors every char to a
        scrolling fb console (`fb_putc`), so all kernel + shell output shows
        on screen. Fixed `fb_scroll` (it never scrolled — cursor clipped at
        the bottom): now scrolls only the text area below a 16 px status
        strip (`FB_HEADER_PX`, 8 color bars drawn by `fb_draw_header`).
        Panic paths call `fb_set_mirror(0)` so the red panic screen stays
        clean (crash test still 97.4% red). Removed the manual
        `fb_puts`/`fb_text` blocks in main.c; the `sched:` summary line is
        printed with IF=0 (a timer preemption used to split it mid-line).
        Screendump now 94k bright pixels / 9 distinct colors. test-boot +
        test-crash PASS.
- [x] **Phase 6: Graphics stack**
  - [x] **6a** — double buffering: all drawing goes to a heap-allocated back
        buffer (`fb_enable_double_buffer()` after heap init), the main loop
        calls `fb_present()` ~50 Hz (back→front copy + dirty flag); panic
        paths call `fb_present()` too so the red crash screen still shows
        (97.4% red verified). New syscalls `SYS_flip(10)`, `SYS_fbinfo(11)`.
        Cursor compositing hook (`fb_set_cursor`/`fb_draw_cursor`) added.
        test-boot + test-crash PASS.
  - [x] **6b** — PS/2 mouse on IRQ12: aux port enabled (0xA8, cfg bit1,
        sample rate 100/s, data reporting 0xF4), 3-byte packets decoded with
        9-bit sign-extended X/Y deltas, own event ring + absolute position;
        **master PIC cascade IRQ2 must be unmasked or slave IRQs never fire**.
        Unified `input_event_t` queue + `SYS_getevent(12)`/`SYS_mousepos(13)`.
        12x19 arrow cursor composited onto the FRONT buffer in `fb_present()`
        (never into the back buffer; `fb_set_cursor` sets `g_dirty` so the
        old position erases). test_boot.py injects `mouse_move`/`mouse_button`
        via the QEMU monitor — serial shows `[mouse] x=1279 y=719 b=01`.
        test-boot + test-crash PASS.
  - [x] **6c** — TrueType renderer (`kernel/lib/ttf.c`): sfnt parser
        (big-endian!), cmap format 4 lookup, simple + composite glyf outlines,
        quadratic bezier flattening (16 samples, int64 De Casteljau), all
        math in 16.16 fixed point (kernel is -mno-sse, no FPU), nonzero-
        winding scanline fill with 4x horizontal subsampling → 8-bit alpha
        bitmaps, 192-entry LRU glyph cache keyed (codepoint, px). DejaVuSans
        (757 KB, OFL, `assets/fonts/LICENSE-DejaVu.txt`) embedded via
        `tools/elf2h.py` → `build/DejaVuSans_ttf.h`. `fb_getpixel`/
        `fb_blendpixel` (src-over) added to fb.c. Boot draws demo strings;
        serial: `ttf: loaded DejaVuSans upm=2048 glyphs=6253`,
        `width at 48px = 424 px`; screendump shows antialiased 48 px
        "MirixOS" glyphs. test-boot + test-crash PASS.
- [x] **Phase 7: Aurora desktop**
  - [x] **7a** — kernel window manager + compositor (`kernel/gui/wm.c`).
        Windows are kernel objects owning a 4 MiB physically-contiguous
        32-bpp buffer, mapped into the owning process at
        `0x40000000 + wid*0x400000` (`vmm_map_in_space()`); the process
        paints content directly and `SYS_win_commit` marks the compositor
        dirty. `wm_render()` draws the desktop into the fb back buffer:
        gradient + aurora bands background (cached), menu bar with
        TrueType "Aurora" label, per-window rounded frame, drop shadow,
        traffic lights, centered TTF title, focus ring, content blit with
        rounded-corner masking. Syscalls 14–21: win_create/commit/move/
        resize/close/focus/top + uptime. `input_pop_filtered()` lets aurora
        consume only mouse events so the mirsh shell keeps the keyboard.
        `user/aurora.c` (ring 3): About/Uptime windows + frameless Dock,
        title-bar drag, red=close / green=zoom traffic lights, Dock hover
        magnification + icon click. QEMU quirk: monitor `mouse_move` is
        relative but its internal absolute cursor is clamped to the screen
        when computing deltas, so out-of-bounds moves are silently lost —
        the test drives in-bounds relative moves from the center (640,360).
        Serial-verified: `aurora: dock hover icon 2 'Notes'`,
        `aurora: drag win 0 -> (210,186)`, `aurora: closed win 1 (Uptime)`;
        screendump shows menu bar, windows, dock. test-boot + test-crash
        PASS.
  - [ ] **7b** — Terminal app: mirsh running inside a window (PTY-style
        bridge between window buffer and shell), plus Files (Navigator)
        over VFS+AHCI and a launcher.
  - [ ] **7c** — C++ widget toolkit + Text Editor / System Settings apps.
- [ ] **Phase 8: Polish & extras** — virtio-net + TCP/IP, SMP, power
      management, optional Rust modules, installer ISO, VirtualBox/real
      hardware testing.

## Definition of done (v1.0)

Boots to the graphical desktop in QEMU, runs multiple user-mode GUI apps at
once, reads/writes its filesystem, takes keyboard+mouse input, and survives a
30-minute stress run without a kernel panic.
