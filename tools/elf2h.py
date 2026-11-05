#!/usr/bin/env python3
"""Convert a binary file (e.g. build/hello.elf) into a C header with a byte
array, so the kernel can embed a user program without a Limine module.
Usage: elf2h.py <input> <symbol> > <output.h>"""
import sys

def main() -> int:
    src, sym = sys.argv[1], sys.argv[2]
    data = open(src, "rb").read()
    out = sys.stdout
    out.write(f"/* Auto-generated from {src} by tools/elf2h.py — do not edit. */\n")
    out.write(f"static const unsigned char {sym}[] = {{\n")
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        out.write("    " + ",".join(f"0x{b:02x}" for b in chunk) + ",\n")
    out.write("};\n")
    out.write(f"static const unsigned long {sym}_len = {len(data)}UL;\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
