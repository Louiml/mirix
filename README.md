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

Expected serial output for the current milestone:

```
MirixOS phase 0: kernel entered (64-bit, higher half)
bootloader: Limine 12.9.6
```

## Layout

```
mirixos/
├── boot/            # limine.conf + Limine binaries (pinned release, see boot/LIMINE_VERSION.txt)
├── kernel/          # arch/x86_64, mm, sched, fs, drivers, syscall, lib
├── userland/        # libc, init, shell, coreutils, gui (later phases)
├── assets/          # fonts/icons/wallpapers (original or OFL only)
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

## Status

See [docs/ROADMAP.md](docs/ROADMAP.md). Currently: **Phase 0 done** — repo,
build system, bootable ISO verified in QEMU.
