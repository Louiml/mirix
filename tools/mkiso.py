#!/usr/bin/env python3
"""Assemble the bootable MirixOS ISO.

Layout inside the ISO (what Limine expects):
    /boot/limine.conf          bootloader config (Limine scans for it on the boot drive)
    /boot/limine-bios-cd.bin   El Torito boot image (BIOS boot)
    /boot/limine-uefi-cd.bin   EFI boot image (UEFI boot)
    /boot/limine-uefi-cd.bin   EFI boot image (UEFI boot)
    /boot/kernel.elf           the kernel
    /EFI/BOOT/BOOTX64.EFI      UEFI application entry point

xorriso writes one ISO that boots both ways: BIOS via El Torito and
UEFI via the EFI System Partition directory on the ISO9660 volume.
"""
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
ISO_ROOT = BUILD / "iso_root"
ISO = BUILD / "mirixos.iso"

BOOT_FILES = ["limine.conf", "limine-bios-cd.bin", "limine-uefi-cd.bin"]
EFI_FILES = ["BOOTX64.EFI", "BOOTIA32.EFI"]


def main() -> None:
    kernel = BUILD / "kernel.elf"
    if not kernel.exists():
        sys.exit("build/kernel.elf missing — run `make` first")

    if ISO_ROOT.exists():
        shutil.rmtree(ISO_ROOT)
    boot = ISO_ROOT / "boot"
    efi = ISO_ROOT / "EFI" / "BOOT"
    boot.mkdir(parents=True)
    efi.mkdir(parents=True)

    for name in BOOT_FILES:
        shutil.copy2(ROOT / "boot" / name, boot / name)
    shutil.copy2(kernel, boot / "kernel.elf")
    for name in EFI_FILES:
        shutil.copy2(ROOT / "boot" / name, efi / name)

    cmd = [
        "xorriso", "-as", "mkisofs",
        "-R", "-r", "-J", "-joliet-long",
        "-b", "boot/limine-bios-cd.bin",
        "-no-emul-boot", "-boot-load-size", "4", "-boot-info-table",
        "--efi-boot", "boot/limine-uefi-cd.bin",
        "-efi-boot-part", "--efi-boot-image",
        "--protective-msdos-label",
        "-o", str(ISO), str(ISO_ROOT),
    ]
    print("+", " ".join(cmd))
    subprocess.run(cmd, check=True)
    print(f"ISO ready: {ISO} ({ISO.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
