# MirixOS

A from-scratch 64-bit hobby operating system: Limine bootloader, higher-half
C kernel (NASM/clang freestanding), user-mode programs, filesystem, and a
macOS-inspired graphical desktop ("Aurora" shell). Built and tested in WSL2
(Ubuntu/Kali) + QEMU.

> The repo lives at `D:\mirixos` on Windows. All builds/tests still run inside
> WSL against `/mnt/c/mirixos` (where the toolchain, QEMU and OVMF are), e.g.
> `wsl -d kali-linux -- bash -c 'cd /mnt/c/mirixos && make test-boot'`.

## Quickstart (inside WSL)

```sh
cd /mnt/c/mirixos        # == D:\mirixos on the Windows side
make test-boot     # build kernel + ISO, boot headless, check serial banner
make run           # interactive UEFI boot: QEMU window (WSLg) + serial on terminal
make run-bios      # same via BIOS/SeaBIOS
make debug         # QEMU paused, waiting for GDB on :1234
```

Expected serial output for the current milestone (Phase 1):

```
MirixOS phase 1: kernel entered (64-bit, higher half)
bootloader: Limine 12.9.6
kernel image: physical_base=0x... virtual_base=0xffffffff80000000
HHDM offset: 0xffff800000000000 ...
memmap: 36 entries ...
usable RAM: 459 MiB
framebuffer: 1024x768 bpp=32 pitch=4096 addr=0xffff800080000000
screen: banner + color bars drawn
```

`make test-boot` also takes a QEMU screendump and verifies the banner and
color bars were actually drawn (prints an ASCII-art rendering of the screen).
`make run` opens the QEMU window via WSLg — you should see "Hello from
MirixOS!" in big letters on a dark background.

## Layout

```
mirixos/
├── boot/            # limine.conf + Limine binaries (pinned release, see boot/LIMINE_VERSION.txt)
├── kernel/          # arch/x86_64, mm, sched, fs, drivers, syscall, lib
├── userland/        # libc, init, shell, coreutils, gui (later phases)
├── assets/          # fonts/icons/wallpapers (original or freely licensed only)
├── tools/           # mkiso.py, test_boot.sh, asset/test scripts
├── tests/           # host-side unit tests + QEMU boot tests
├── docs/            # ROADMAP.md (checklist), DESIGN.md (memory map, syscall table, drivers)
└── Makefile
```

## Toolchain

clang + ld.lld (freestanding, `-mcmodel=kernel`), NASM, GNU Make, Python 3,
xorriso, mtools, QEMU (`qemu-system-x86_64`), OVMF, GDB. On Kali/Debian:

```sh
sudo apt install clang lld nasm make python3 xorriso mtools qemu-system-x86 ovmf gdb
```

## Run the ISO in a virtual machine

The bootable image is **`build/mirixos.iso`** (~5.7 MB, hybrid: boots via UEFI
**and** legacy BIOS/El Torito — verified with OVMF and SeaBIOS on q35 and
i440fx). On Windows that is `D:\mirixos\build\mirixos.iso`. Rebuild anytime with:

```sh
wsl -d kali-linux -- bash -c 'cd /mnt/d/mirixos && make iso'
```

- **QEMU**: `qemu-system-x86_64 -machine q35 -m 512M -smp 4 -cdrom build/mirixos.iso`
  (add `-drive if=none,id=hd0,file=build/mirix.img,format=raw -device ide-hd,drive=hd0,bus=ide.0`
  to attach the AHCI test disk; `make run` does UEFI+OVMF with a GUI window)
- **VirtualBox**: create a new VM (OS type "Other/Unknown", **64-bit**,
  >= 512 MB RAM, >= 4 CPUs), attach `mirixos.iso` as the optical drive, boot.
  Enable UEFI *or* leave BIOS — both work. Required VM settings for input:
  - Settings → System → **Pointing Device = "PS/2 Mouse"** (the default USB
    tablet needs Guest Additions, which a custom OS doesn't have; the
    "mouse pointer integration not supported" popup is expected and harmless).
  - Settings → System → Motherboard: leave **"Enable I/O APIC" on** (VirtualBox
    turns it on automatically for >1 vCPU). MirixOS detects it via the ACPI
    MADT and routes keyboard IRQ1 / mouse IRQ12 through the IO-APIC; with 1 vCPU
    or the legacy PIC it falls back automatically (`force_pic` cmdline forces
    PIC for tests).
  - Click inside the VM display to **capture** the mouse/keyboard; **Right
    Ctrl** releases them.
  - Settings → Ports → Serial Ports: enable **Port 1**, mode **Raw File**,
    path e.g. `C:\mirix-serial.log`. Any kernel panic prints
    `exception: <name> (vector N, error 0x…) at rip=… cpu=… proc=…` + `cr2=…`
    there before the red screen — send that log if anything still crashes.
  The Aurora desktop comes up at **1024x768** (a mode VirtualBox VBE always
  has; override with `MIRIXOS_RESOLUTION=1280x720x32 make iso`). **Maximize the
  VM window** (or use View → Fullscreen/Scale) so the desktop isn't downscaled —
  that is what made the text hard to read. Click the dock icons (About / Clock /
  Files / Terminal / Editor / Settings).

## Tests

```sh
make test-boot       # full scripted desktop drive on a force_pic ISO (serial + screendump + cursor/history)
make test-ioapic     # Phase 9b: default ISO on q35 — IO-APIC active, keyboard/mouse/history, 0 exceptions
make test-crash      # panic path: crash-cmdline ISO, expects #PF red panic screen
make test-poweroff   # ACPI S5: poweroff-cmdline ISO, QEMU must exit on its own
make test-stress     # Definition-of-done: 30 min input-hammering stress run (-smp 4)
make test-stress STRESS_SECONDS=90   # quick 90 s smoke run
```

All tests run QEMU headless with `-smp 4` (SMP), OVMF UEFI, an AHCI disk
image and the default e1000 NIC on slirp.

## Installing / live USB

`build/mirixos.iso` is a hybrid BIOS+UEFI boot image. To make a bootable USB:

```sh
make iso
sudo tools/burn.sh /dev/sdX        # asks for confirmation; --dry-run to preview
```

## Status

See [docs/ROADMAP.md](docs/ROADMAP.md). Phases 0-9a done: Limine boot (UEFI +
BIOS), higher-half kernel, PMM/VMM/heap, preemptive per-CPU SMP scheduler
(`-smp 4`), syscalls, ring-3 processes with per-process address spaces, VFS +
ramdisk, AHCI disk, PS/2 input, double-buffered framebuffer + TrueType, a
kernel window manager/compositor, the ring-3 **Aurora** desktop (dock, drag,
traffic lights, Terminal via pty, Files, Text Editor + Settings C++ apps),
e1000 networking, ACPI S5 poweroff, and a 30-minute stress harness
(`make test-stress` — PASSED 2026-11-06: 0 exceptions, 285k context
switches over 30 min on `-smp 4`, desktop still rendering at the end).
Phase 9a (interactive input) done: hardware cursor follows the PS/2 mouse,
arrow keys + shell history (UP/DOWN) work in the Terminal, keyboard
routing follows window focus — `make test-boot` PASS x3.
Phase 9b (VirtualBox compatibility) done: IO-APIC interrupt routing (MADT) with
PIC fallback, robust PS/2 IRQ draining, window clamping to the real
framebuffer, `history` shell command — `make test-boot` (force_pic drive)
PASS and `make test-ioapic` PASS, 0 exceptions, SeaBIOS boot also clean.
Definition of done: all met (see docs/ROADMAP.md).
