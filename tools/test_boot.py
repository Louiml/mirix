#!/usr/bin/env python3
"""Headless boot test for MirixOS.

Boots build/mirixos.iso in QEMU with no display, then verifies:
  1. serial output contains the kernel banner + bootloader line + fb line
  2. a monitor `screendump` of the framebuffer shows rendered text and
     color bars (bright-pixel count and distinct-color count)
Also prints a small ASCII-art rendering of the top-left screen region so
the result is visible in the log.
"""
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
ISO = BUILD / "mirixos.iso"
SERIAL = BUILD / "serial.log"
SHOT = BUILD / "screen.ppm"
# NOTE: AF_UNIX sockets cannot live on the drvfs (/mnt/*) mount, so the
# QEMU monitor socket goes to /tmp and the screendump is copied back.
MON = Path("/tmp/mirixos-mon.sock")
SHOT_TMP = Path("/tmp/mirixos-screen.ppm")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"

BANNER = "MirixOS phase 4a: kernel entered"


def strip_ansi(data: bytes) -> str:
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    text = re.sub(r"\x1b[()][0-9A-B]", "", text)
    text = re.sub(r"\x1b[=>]", "", text)
    text = re.sub(r"[\x00-\x08\x0b-\x1f]", "", text)
    return text


def read_ppm(path: Path):
    """Parse a binary P6 PPM -> (width, height, bytes)."""
    data = path.read_bytes()
    # header: P6 <w> <h> <maxval> then a single whitespace, then raster
    m = re.match(rb"P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s", data)
    if not m:
        raise ValueError("not a P6 PPM")
    w, h, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    raster = data[m.end():]
    return w, h, maxval, raster


def ascii_art(w, h, raster, x0=0, y0=0, x1=None, y1=None, cell=4):
    x1 = x1 if x1 is not None else min(w, 720)
    y1 = y1 if y1 is not None else min(h, 128)
    lines = []
    for cy in range(y0, y1, cell):
        row = []
        for cx in range(x0, x1, cell):
            bright = False
            for y in range(cy, min(cy + cell, h)):
                base = (y * w + cx) * 3
                for x in range(cx, min(cx + cell, w)):
                    r, g, b = raster[base], raster[base + 1], raster[base + 2]
                    if r + g + b > 380:
                        bright = True
                        break
                base += cell * 3
                if bright:
                    break
            row.append("#" if bright else " ")
        lines.append("".join(row).rstrip())
    return "\n".join(lines)


def main() -> int:
    if not ISO.exists():
        print("build/mirixos.iso missing — run `make iso` first", file=sys.stderr)
        return 2
    for f in (SERIAL, SHOT, MON, SHOT_TMP, BUILD / "OVMF_VARS.fd"):
        if f.exists():
            f.unlink()
    shutil.copy2(OVMF_VARS, BUILD / "OVMF_VARS.fd")

    cmd = [
        "qemu-system-x86_64",
        "-machine", "q35", "-cpu", "qemu64", "-m", "512M",
        "-cdrom", str(ISO),
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={BUILD / 'OVMF_VARS.fd'}",
        "-display", "none",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"unix:{MON},server,nowait",
        "-no-reboot",
    ]
    print("Booting headless:", " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        # Wait until the kernel banner shows up on serial (max 30s).
        booted = False
        for _ in range(300):
            try:
                if SERIAL.exists() and BANNER in strip_ansi(SERIAL.read_bytes()):
                    booted = True
                    break
            except OSError:
                pass  # serial file mid-write on drvfs; retry next poll
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        # Wait until both user processes have exited (or a panic/halt, max 30s)
        # so their output is captured before the screendump.
        for _ in range(300):
            try:
                if SERIAL.exists():
                    s = strip_ansi(SERIAL.read_bytes())
                    if s.count("[user ") >= 2 and "exit(" in s or "halting." in s:
                        break
            except OSError:
                pass
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        time.sleep(0.5)  # let the kernel finish drawing

        # QEMU monitor: inject keystrokes into the emulated PS/2 keyboard,
        # wait for the kernel's [kbd] echo lines, then screendump.
        if MON.exists() and booted:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(5)
            s.connect(str(MON))
            for key in ("h", "i", "spc", "ret"):
                s.sendall(f"sendkey {key}\n".encode())
                try:
                    s.recv(4096)
                except socket.timeout:
                    pass
                time.sleep(0.3)
            kbd_marks = ("[kbd] 'h'", "[kbd] 'i'", "[kbd] SPACE", "[kbd] ENTER")
            for _ in range(100):
                try:
                    if SERIAL.exists():
                        cur = strip_ansi(SERIAL.read_bytes())
                        if all(m in cur for m in kbd_marks):
                            break
                except OSError:
                    pass
                time.sleep(0.1)
            s.sendall(f"screendump {SHOT_TMP}\n".encode())
            try:
                s.recv(4096)
            except socket.timeout:
                pass
            time.sleep(0.5)
            s.sendall(b"quit\n")
            s.close()
            if SHOT_TMP.exists():
                shutil.copy2(SHOT_TMP, SHOT)
    finally:
        if proc.poll() is None:
            proc.terminate()
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()

    serial = strip_ansi(SERIAL.read_bytes()) if SERIAL.exists() else ""
    print("--- serial output (ANSI stripped, tail) ---")
    print("\n".join(serial.splitlines()[-25:]))
    print("-------------------------------------------")

    m = re.search(r"timer: (\d+) ticks in (\d+) us", serial)
    # 200 ms window at 100 Hz should yield ~20 ticks; allow TCG jitter.
    ok_timer = m is not None and 5 <= int(m.group(1)) <= 80
    m2 = re.search(r"sched: (\d+) switches, (\d+) ticks", serial)
    # both demo threads + main must have run, and the scheduler must have
    # switched at least twice (timer-driven preemption)
    ok_sched = (
        m2 is not None
        and int(m2.group(1)) >= 2
        and "[alpha]" in serial
        and "[beta]" in serial
        and "[main]" in serial
    )
    ok_user = (
        "syscall: STAR=" in serial
        and "elf: loaded" in serial
        and serial.count("spawned user process") >= 2   # banner may interleave with user output
        and "P3." in serial
        and "P4." in serial
        and "[user 3] exit(43)" in serial
        and "[user 4] exit(44)" in serial
    )
    m3 = re.search(r"pci: enumerated (\d+) devices", serial)
    ok_pci = (
        m3 is not None
        and int(m3.group(1)) >= 5
        and re.search(r"class=01\.", serial) is not None   # AHCI/SATA
        and re.search(r"class=02\.", serial) is not None   # NIC
    )
    ok_kbd = (
        "ps2: keyboard ready" in serial
        and "[kbd] 'h'" in serial
        and "[kbd] 'i'" in serial
        and "[kbd] SPACE" in serial
        and "[kbd] ENTER" in serial
    )
    ok_serial = (
        BANNER in serial
        and "bootloader: Limine" in serial
        and re.search(r"framebuffer: \d+x\d+ bpp=32", serial) is not None
        and "pmm:" in serial
        and "heap self-test: PASS" in serial
        and "vmm: map/unmap roundtrip PASS" in serial
        and "tsc calibrated" in serial
        and "lapic: timer started" in serial
        and ok_timer
        and ok_sched
        and ok_user
        and ok_pci
        and ok_kbd
    )

    ok_screen = False
    if SHOT.exists():
        w, h, maxval, raster = read_ppm(SHOT)
        bright = 0
        colors = set()
        for i in range(0, len(raster) - 2, 3):
            r, g, b = raster[i], raster[i + 1], raster[i + 2]
            if r + g + b > 500:
                bright += 1
            colors.add((r >> 4, g >> 4, b >> 4))
        print(f"screendump: {w}x{h}, bright pixels={bright}, distinct colors={len(colors)}")
        print("--- top-left of screen (ASCII art) ---")
        print(ascii_art(w, h, raster))
        print("--------------------------------------")
        ok_screen = bright > 1500 and len(colors) >= 6
    else:
        print("screendump: MISSING (kernel did not boot in time or monitor failed)")

    print(f"serial check: {'PASS' if ok_serial else 'FAIL'}")
    print(f"screen check: {'PASS' if ok_screen else 'FAIL'}")
    if ok_serial and ok_screen:
        print("BOOT TEST: PASS")
        return 0
    print("BOOT TEST: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
