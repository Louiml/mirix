#!/usr/bin/env python3
"""30-minute (default) stress test for MirixOS — Definition-of-done gate.

Boots build/mirixos.iso headless in QEMU (-smp 4), waits for the Aurora
desktop, then hammers the input path through the QEMU monitor for
STRESS_SECONDS (default 1800): sweeping mouse moves, dock/desktop clicks and
keystrokes (routed to the shell pty or the Editor, whichever is focused).

Fails fast if the serial log ever shows `exception:`, `KERNEL PANIC` or
`halting.`. At the end requires:
  - zero exceptions / panics in build/serial.log
  - the `[main] alive: N switches, M ticks` counter advanced (the kernel
    kept scheduling for the whole run)
  - a final monitor screendump still shows a rendered desktop

Usage: STRESS_SECONDS=1800 python3 tools/test_stress.py   (make test-stress)
       STRESS_SECONDS=90   python3 tools/test_stress.py   (smoke test)
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
ISO = BUILD / "mirixos.iso"
SERIAL = BUILD / "serial.log"
SHOT = BUILD / "screen-stress.ppm"
MON = Path("/tmp/mirixos-mon.sock")
SHOT_TMP = Path("/tmp/mirixos-stress.ppm")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
HD = BUILD / "mirix.img"

SECONDS = int(os.environ.get("STRESS_SECONDS", "1800"))

READY = "aurora: ready: 4 windows"


def strip_ansi(data: bytes) -> str:
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)
    text = re.sub(r"\x1b[()][0-9A-B]", "", text)
    text = re.sub(r"\x1b[=>]", "", text)
    return re.sub(r"[\x00-\x08\x0b-\x1f]", "", text)


def serial_text() -> str:
    try:
        if SERIAL.exists():
            return strip_ansi(SERIAL.read_bytes())
    except OSError:
        pass
    return ""


def alive_ticks(text: str):
    """Parse the last '[main] alive: N switches, M ticks' line."""
    m = re.findall(r"\[main\] alive: (\d+) switches, (\d+) ticks", text)
    if not m:
        return None
    return int(m[-1][0]), int(m[-1][1])


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
    print(f"Stress boot ({SECONDS}s) headless:", " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def bail(msg):
        print(f"STRESS FAIL: {msg}")
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
        return 1

    try:
        # 1. wait for the desktop (max 60 s)
        t0 = time.time()
        while time.time() - t0 < 60:
            if READY in serial_text():
                break
            if proc.poll() is not None:
                return bail("QEMU exited before aurora was ready")
            time.sleep(0.2)
        else:
            return bail(f"aurora never reported ready ({READY!r} missing)")
        print(f"desktop ready after {time.time() - t0:.1f}s")

        # 2. connect the monitor
        s = socket.socket(socket.AF_UNIX)
        s.settimeout(5)
        for _ in range(50):
            if MON.exists():
                try:
                    s.connect(str(MON))
                    break
                except OSError:
                    pass
            time.sleep(0.1)
        else:
            return bail("monitor socket never appeared")

        def mcmd(c):
            s.sendall((c + "\n").encode())
            try:
                s.recv(4096)
            except socket.timeout:
                pass

        # 3. hammer input for SECONDS
        start = time.time()
        first_alive = None
        last_alive = None
        n_moves = n_clicks = n_keys = 0
        dock_xs = [495, 553, 611, 669, 727, 785]   # 6 dock icon centers, y=670
        letters = ["a", "b", "c", "d", "e", "h", "i", "l", "o", "s", "t"]
        i = 0
        while time.time() - start < SECONDS:
            if proc.poll() is not None:
                return bail("QEMU exited mid-stress")
            cur = serial_text()
            if "exception:" in cur or "KERNEL PANIC" in cur or "halting." in cur:
                return bail("kernel exception/panic during stress — see serial tail below")
            a = alive_ticks(cur)
            if a is not None:
                if first_alive is None:
                    first_alive = a
                last_alive = a

            # sweep the mouse across the screen and the dock
            x = 40 + (i * 37) % 1200
            y = 40 + (i * 53) % 640
            mcmd(f"mouse_move {x - 640} {y - 360}")
            n_moves += 1
            if i % 10 == 0:                       # click a dock icon
                dx = dock_xs[(i // 10) % len(dock_xs)]
                mcmd(f"mouse_move {dx - 640} {670 - 360}")
                mcmd("mouse_button 1")
                mcmd("mouse_button 0")
                n_clicks += 1
            if i % 7 == 0:                        # type into shell/editor
                mcmd(f"sendkey {letters[i % len(letters)]}")
                n_keys += 1
            if i % 25 == 0:
                mcmd("sendkey ret")
                mcmd("sendkey backspace")
                n_keys += 2
            i += 1
            time.sleep(2.0)

        # 4. final checks
        cur = serial_text()
        exc = cur.count("exception:")
        panic = "KERNEL PANIC" in cur or "halting." in cur
        alive_ok = first_alive is not None and last_alive is not None and last_alive[1] > first_alive[1]
        # screendump: desktop still rendering?
        if SHOT_TMP.exists():
            SHOT_TMP.unlink()
        mcmd(f"screendump {SHOT_TMP}")
        time.sleep(1.0)
        bright = 0
        try:
            if SHOT_TMP.exists():
                shutil.copy2(SHOT_TMP, SHOT)
                data = SHOT.read_bytes()
                m = re.match(rb"P6\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s+(?:#[^\n]*\n\s*)*(\d+)\s", data)
                if m:
                    w, h = int(m.group(1)), int(m.group(2))
                    raster = data[m.end():]
                    step = 997 * 3
                    for off in range(0, min(len(raster), w * h * 3), step):
                        if raster[off] + raster[off + 1] + raster[off + 2] > 380:
                            bright += 1
        except OSError:
            pass

        print(f"stress ran {time.time() - start:.0f}s: {n_moves} mouse moves, "
              f"{n_clicks} clicks, {n_keys} keys")
        print(f"exceptions={exc} panic={panic} alive {first_alive} -> {last_alive} "
              f"screen_bright_cells={bright}")
        ok = (exc == 0 and not panic and alive_ok and bright > 0)
        print("STRESS TEST:", "PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        try:
            s.close()
        except OSError:
            pass
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
