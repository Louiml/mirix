#!/usr/bin/env python3
"""Panic-path test: boot build/mirixos-crash.iso (kernel cmdline "crash",
which makes the kernel dereference NULL) and verify the CPU exception path:
  1. serial shows the exception line + KERNEL PANIC + register dump (#PF, cr2=0x0)
  2. the screendump is dominated by panic-red pixels (red panic screen)
Run after `make test-crash` (which builds the crash ISO first).
"""
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_boot import strip_ansi, read_ppm  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
ISO = BUILD / "mirixos-crash.iso"
SERIAL = BUILD / "serial-crash.log"
SHOT_TMP = Path("/tmp/mirixos-crash.ppm")
SHOT = BUILD / "screen-crash.ppm"
MON = Path("/tmp/mirixos-crash-mon.sock")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"


def main() -> int:
    if not ISO.exists():
        print("build/mirixos-crash.iso missing — run `make test-crash`", file=sys.stderr)
        return 2
    for f in (SERIAL, SHOT, SHOT_TMP, MON, BUILD / "OVMF_VARS.fd"):
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
    print("Booting crash ISO headless:", " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        panicked = False
        for _ in range(300):
            if SERIAL.exists() and "KERNEL PANIC" in strip_ansi(SERIAL.read_bytes()):
                panicked = True
                break
            if proc.poll() is not None:
                break
            time.sleep(0.1)
        time.sleep(1.0)
        if MON.exists() and panicked:
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(5)
            s.connect(str(MON))
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
    print("--- serial (tail) ---")
    print("\n".join(serial.splitlines()[-20:]))
    print("---------------------")

    ok_serial = (
        "#PF page fault" in serial
        and "KERNEL PANIC" in serial
        and "cr2=0x0" in serial
        and "rip=" in serial
    )

    ok_screen = False
    if SHOT.exists():
        w, h, maxval, raster = read_ppm(SHOT)
        red = 0
        total = w * h
        for i in range(0, len(raster) - 2, 3):
            r, g, b = raster[i], raster[i + 1], raster[i + 2]
            if r > 100 and g < 90 and b < 90:
                red += 1
        frac = red / total
        print(f"screendump: {w}x{h}, panic-red fraction={frac:.2%}")
        ok_screen = frac > 0.5
    else:
        print("screendump: MISSING")

    print(f"serial check: {'PASS' if ok_serial else 'FAIL'}")
    print(f"screen check: {'PASS' if ok_screen else 'FAIL'}")
    if ok_serial and ok_screen:
        print("CRASH TEST: PASS")
        return 0
    print("CRASH TEST: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
