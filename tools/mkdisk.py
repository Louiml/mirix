#!/usr/bin/env python3
"""Create build/mirix.img — a raw 64 MiB disk image for the AHCI test.

LBA 0   sector 0:  magic "MIRIXAHCI0" + boot-sector-ish text
LBA 100 sector 100: magic "MIRIXLBA100" + pattern
Everything else zero (sparse).
"""
import sys
from pathlib import Path

SIZE = 64 * 1024 * 1024

LBA0_MAGIC = b"MIRIXAHCI0"
LBA0_TEXT = b"MirixOS AHCI disk test - boot sector pattern 0123456789 ABCDEF"
LBA100_MAGIC = b"MIRIXLBA100"
LBA100_TEXT = b"MirixOS LBA100 pattern: the quick brown fox jumps over the lazy dog"


def main() -> int:
    out = Path(sys.argv[1] if len(sys.argv) > 1 else "build/mirix.img")
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "wb") as f:
        f.truncate(SIZE)
        f.seek(0)
        sec0 = LBA0_MAGIC + LBA0_TEXT
        f.write(sec0.ljust(512, b"\0"))
        f.seek(100 * 512)
        sec100 = LBA100_MAGIC + LBA100_TEXT
        f.write(sec100.ljust(512, b"\0"))
    print(f"mkdisk: wrote {out} ({SIZE // (1024*1024)} MiB, LBA0 magic {LBA0_MAGIC!r}, LBA100 magic {LBA100_MAGIC!r})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
