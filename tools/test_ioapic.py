#!/usr/bin/env python3
"""Light IO-APIC smoke test (Phase 9b).

Boots the DEFAULT ISO (no force_pic) on q35, where the ACPI MADT advertises an
I/O APIC, so the kernel routes IRQ1/IRQ12 through the IO-APIC instead of the
8259 PIC. We only require: boot with zero exceptions, IO-APIC active, desktop
ready, keyboard-driven shell commands (ls/ps/history) round-trip, and at least
one PS/2 mouse packet. The full click-through desktop drive runs on the
force_pic ISO via `make test-boot` (TCG makes IO-APIC packet timing flaky for
scripted absolute mouse moves, but real hardware/VirtualBox is fine).
"""
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
ISO = BUILD / os.environ.get("MIRIXOS_ISO_NAME", "mirixos.iso")
SERIAL = BUILD / "serial.log"
MON = Path("/tmp/mirixos-mon.sock")
SHOT_TMP = Path("/tmp/mirixos-ioapic.ppm")
SHOT = BUILD / "screen-ioapic.ppm"
OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
HD = BUILD / "mirix.img"
BANNER = "MirixOS phase 8c: kernel entered"


def strip_ansi(data: bytes) -> str:
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    text = re.sub(r"\x1b[()][0-9A-B]", "", text)
    text = re.sub(r"\x1b[=>]", "", text)
    return re.sub(r"[\x00-\x08\x0b-\x1f]", "", text)


def wait_for(marks, tries=300):
    for _ in range(tries):
        try:
            if SERIAL.exists():
                cur = strip_ansi(SERIAL.read_bytes())
                if all(m in cur for m in marks):
                    return True
        except OSError:
            pass
        time.sleep(0.1)
    return False


def main() -> int:
    if not ISO.exists():
        print("missing ISO — run `make iso` first", file=sys.stderr)
        return 2
    for f in (SERIAL, MON, SHOT_TMP, BUILD / "OVMF_VARS.fd"):
        if f.exists():
            f.unlink()
    shutil.copy2(OVMF_VARS, BUILD / "OVMF_VARS.fd")
    cmd = [
        "qemu-system-x86_64",
        "-machine", "q35", "-cpu", "qemu64", "-smp", "4", "-m", "512M",
        "-cdrom", str(ISO),
        "-drive", f"if=pflash,format=raw,readonly=on,file={OVMF_CODE}",
        "-drive", f"if=pflash,format=raw,file={BUILD / 'OVMF_VARS.fd'}",
        "-drive", f"if=none,id=hd0,file={HD},format=raw",
        "-device", "ide-hd,drive=hd0,bus=ide.0",
        "-display", "none",
        "-serial", f"file:{SERIAL}",
        "-monitor", f"unix:{MON},server,nowait",
        "-no-reboot",
    ]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for((BANNER,), tries=300):
            print("kernel banner not seen", file=sys.stderr)
            return 2
        # desktop up
        wait_for(("aurora: ready: 4 windows", "mirsh ready"), tries=600)
        if MON.exists():
            s = socket.socket(socket.AF_UNIX)
            s.settimeout(5)
            s.connect(str(MON))

            def send_keys(keys):
                for key in keys:
                    s.sendall(f"sendkey {key}\n".encode())
                    try:
                        s.recv(4096)
                    except socket.timeout:
                        pass
                    time.sleep(0.25)

            send_keys(("l", "s", "ret"))
            wait_for(("ls: 1 entries",))
            send_keys(("p", "s", "ret"))
            wait_for(("name=shell", "name=userA"))
            send_keys(("h", "i", "s", "t", "o", "r", "y", "ret"))
            wait_for(("hist[",), tries=200)
            # one mouse move — a PS/2 packet must arrive via the IO-APIC
            s.sendall(b"mouse_move 120 80\n")
            try:
                s.recv(4096)
            except socket.timeout:
                pass
            wait_for(("[mouse] x=",), tries=200)
            time.sleep(0.5)
            if SHOT_TMP.exists():
                SHOT_TMP.unlink()
            s.sendall(f"screendump {SHOT_TMP}\n".encode())
            try:
                s.recv(4096)
            except socket.timeout:
                pass
            time.sleep(0.8)
            try:
                s.sendall(b"quit\n")
                s.close()
            except OSError:
                pass
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
    ok_ioapic = "ioapic: phys" in serial and "pin1->vec 33" in serial
    ok_boot = BANNER in serial and "aurora: ready: 4 windows" in serial and "mirsh ready" in serial
    ok_keys = "ls: 1 entries" in serial and "name=shell" in serial and "hist[" in serial
    ok_mouse = "[mouse] x=" in serial
    ok_clean = "exception:" not in serial and "KERNEL PANIC" not in serial
    ok_screen = False
    if SHOT.exists():
        data = SHOT.read_bytes()
        m = re.match(rb"P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s", data)
        if m:
            raster = data[m.end():]
            bright = 0
            colors = set()
            for i in range(0, len(raster) - 2, 3):
                r, g, b = raster[i], raster[i + 1], raster[i + 2]
                if r + g + b > 500:
                    bright += 1
                colors.add((r >> 4, g >> 4, b >> 4))
            ok_screen = bright > 1500 and len(colors) >= 6
            print(f"screendump: bright={bright} colors={len(colors)}")
    for name, ok in [("ioapic", ok_ioapic), ("boot", ok_boot), ("keys", ok_keys),
                     ("mouse", ok_mouse), ("clean", ok_clean), ("screen", ok_screen)]:
        print(f"ioapic {name}: {'PASS' if ok else 'FAIL'}")
    if all([ok_ioapic, ok_boot, ok_keys, ok_mouse, ok_clean, ok_screen]):
        print("IO-APIC TEST: PASS")
        return 0
    print("IO-APIC TEST: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
