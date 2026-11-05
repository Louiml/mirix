#!/bin/bash
# Headless boot test: boot the ISO in QEMU with no display, capture serial
# output, and check the kernel banner appears. Exit 0 = PASS.
set -u
cd "$(dirname "$0")/.."

OVMF_CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
OVMF_VARS_SRC=/usr/share/OVMF/OVMF_VARS_4M.fd
mkdir -p build
cp -f "$OVMF_VARS_SRC" build/OVMF_VARS.fd
rm -f build/serial.log

echo "Booting build/mirixos.iso headless (25s cap)..."
timeout 25 qemu-system-x86_64 \
    -machine q35 -cpu qemu64 -m 512M \
    -cdrom build/mirixos.iso \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
    -display none -serial file:build/serial.log -no-reboot
# timeout(124) is expected: the kernel halts in a hlt loop forever.

echo "--- serial output (build/serial.log) ---"
cat build/serial.log
echo "---------------------------------------"
if grep -q "MirixOS phase 0: kernel entered" build/serial.log \
   && grep -q "bootloader: Limine" build/serial.log; then
    echo "BOOT TEST: PASS"
    exit 0
else
    echo "BOOT TEST: FAIL (banner missing — see serial log above)"
    exit 1
fi
