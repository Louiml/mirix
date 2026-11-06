#!/usr/bin/env python3
"""Poweroff-path test (Phase 8b): boot build/mirixos-poweroff.iso (kernel
cmdline "poweroff", which parses ACPI RSDP/XSDT/FADT and writes S5 to
PM1a_CNT) and verify:
  1. serial shows the ACPI markers and the 'acpi: poweroff' line
  2. QEMU actually EXITS (S5 shutdown with -no-reboot => process exit),
     i.e. a clean poweroff, not a hang or panic.
Run after `make test-poweroff` (which builds the poweroff ISO first).
"""
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from test_boot import strip_ansi  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"
ISO = BUILD / "mirixos-poweroff.iso"
SERIAL = BUILD / "serial-poweroff.log"
MON = Path("/tmp/mirixos-poweroff-mon.sock")

OVMF_CODE = "/usr/share/OVMF/OVMF_CODE_4M.fd"
OVMF_VARS = "/usr/share/OVMF/OVMF_VARS_4M.fd"
HD = BUILD / "mirix.img"


def main() -> int:
    if not ISO.exists():
        print("build/mirixos-poweroff.iso missing — run `make test-poweroff`",
              file=sys.stderr)
        return 2
    for f in (SERIAL, MON, BUILD / "OVMF_VARS.fd"):
        if f.exists():
            f.unlink()
    shutil.copy2(OVMF_VARS, BUILD / "OVMF_VARS.fd")

    cmd = [
        "qemu-system-x86_64",
        "-machine", "q35", "-cpu", "qemu64", "-m", "512M",
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
    print("Booting poweroff ISO headless:", " ".join(cmd))
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    exited = False
    try:
        # Wait up to 40s for QEMU to exit on its own (clean S5 shutdown)
        for _ in range(400):
            if proc.poll() is not None:
                exited = True
                break
            time.sleep(0.1)
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()

    serial = strip_ansi(SERIAL.read_bytes()) if SERIAL.exists() else ""
    print("--- serial (tail) ---")
    print("\n".join(serial.splitlines()[-15:]))
    print("---------------------")
    print(f"qemu exited on its own: {'YES' if exited else 'NO'} (code {proc.returncode})")

    ok_serial = (
        "acpi: FADT" in serial
        and "PM1a_CNT port" in serial
        and "cmdline: 'poweroff'" in serial
        and "acpi: poweroff" in serial
        and "KERNEL PANIC" not in serial
    )
    ok_exit = exited and proc.returncode == 0

    print(f"serial check: {'PASS' if ok_serial else 'FAIL'}")
    print(f"qemu-exit check: {'PASS' if ok_exit else 'FAIL'}")
    if ok_serial and ok_exit:
        print("POWEROFF TEST: PASS")
        return 0
    print("POWEROFF TEST: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
