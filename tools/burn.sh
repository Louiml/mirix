#!/bin/bash
# burn.sh — write the MirixOS ISO to a USB stick / disk (the "installer").
#
# The ISO (build/mirixos.iso) is a hybrid BIOS+UEFI boot medium: burning it
# to a USB stick makes a bootable MirixOS installer/live USB. On real
# hardware, boot it (UEFI or legacy BIOS), and the Aurora desktop comes up.
#
# Usage:
#   sudo tools/burn.sh /dev/sdX            # burn (asks for confirmation)
#   sudo tools/burn.sh /dev/sdX --yes      # burn without asking
#   tools/burn.sh /dev/sdX --dry-run       # show what would happen
#
# Safety: refuses to touch a mounted device, the root device, or a device
# smaller than the ISO.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
ISO="$ROOT_DIR/build/mirixos.iso"

DEV=""
ASSUME_YES=0
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --yes)     ASSUME_YES=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *)         DEV="$arg" ;;
    esac
done

if [ -z "$DEV" ]; then
    echo "usage: sudo $0 <device> [--yes|--dry-run]   e.g. $0 /dev/sdX" >&2
    echo "       (list candidates: lsblk)" >&2
    exit 2
fi
if [ ! -f "$ISO" ]; then
    echo "error: $ISO not found — run 'make iso' first" >&2
    exit 2
fi
if [ ! -b "$DEV" ]; then
    echo "error: $DEV is not a block device" >&2
    exit 2
fi

ISO_BYTES=$(stat -c %s "$ISO")
DEV_BYTES=$(blockdev --getsize64 "$DEV" 2>/dev/null || echo 0)
ROOT_DEV=$(findmnt -n -o SOURCE / 2>/dev/null || true)

# --- safety checks -------------------------------------------------------
if [ "$DEV" = "$ROOT_DEV" ]; then
    echo "error: $DEV is the root device — refusing" >&2
    exit 2
fi
if findmnt -n -o SOURCE "$DEV" >/dev/null 2>&1 || \
   grep -q "^$DEV" /proc/mounts 2>/dev/null || \
   findmnt -rn -o SOURCE | grep -q "^$DEV"; then
    echo "error: $DEV (or a partition of it) is MOUNTED — unmount it first:" >&2
    findmnt -rn -o TARGET,SOURCE | grep "$DEV" || true
    exit 2
fi
if [ "$DEV_BYTES" -lt "$ISO_BYTES" ]; then
    echo "error: $DEV is $DEV_BYTES bytes, ISO needs $ISO_BYTES — too small" >&2
    exit 2
fi
# refuse whole-disk devices that look like the Windows system disk via lsblk? (Linux-only env; skip)

echo "About to write:"
echo "  image:   $ISO ($ISO_BYTES bytes, $(du -h "$ISO" | cut -f1))"
echo "  device:  $DEV ($DEV_BYTES bytes, $(lsblk -d -o SIZE "$DEV" 2>/dev/null | tail -1))"
echo "  WARNING: everything on $DEV will be ERASED."
if [ "$DRY_RUN" = "1" ]; then
    echo "  (dry run — nothing written)"
    exit 0
fi
if [ "$ASSUME_YES" = "0" ]; then
    read -r -p "Type the device name ($DEV) to confirm: " CONFIRM
    if [ "$CONFIRM" != "$DEV" ]; then
        echo "aborted (typed '$CONFIRM')" >&2
        exit 2
    fi
fi

echo "Writing..."
dd if="$ISO" of="$DEV" bs=4M conv=fsync status=progress
echo "Syncing..."
sync
echo "DONE. Remove the device and boot MirixOS from it (UEFI or legacy BIOS)."
