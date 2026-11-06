#!/usr/bin/env python3
"""Audit .vitalink.fstubs.* sections for symbols that do not belong there.

WHY
===
vita-elf-create turns every still-UNDEFINED symbol into a 16-byte import
stub (00000008 / hash / hash / nop) and files it under a *Sce* stub group.
At load time the kernel resolves each stub against the Sce modules that are
actually linked; a symbol no module exports resolves to 0, so the FIRST CALL
executes address 0 -> prefetch abort -> psp2core dump with pc=0.

That is exactly how v04.04 was found (log174): newlib's `getenv` object was
not pulled in (only `_getenv_r` was), so `getenv` became a stub inside
`.vitalink.fstubs.SceLibMonoBridge`, and libstdc++'s static initialisation
called it from .init_array before any of this port's code ran.

A stub is fine for real Sce APIs (sceKernel*, pss_code_mem_*, taiHook*).
It is fatal for anything else.

USAGE
    python3 debug/tools/check_vitalink_stubs.py [elf]

Default ELF: vita-port/build/cmake/midp_vita
Exit code 0 = clean, 1 = suspicious symbol found.
"""
import re
import subprocess
import sys

ELF = sys.argv[1] if len(sys.argv) > 1 else "vita-port/build/cmake/midp_vita"

# Names that legitimately live in a Sce stub group.
OK_PREFIXES = ("sce", "_sce", "pss_", "tai")


def main() -> int:
    try:
        sections = subprocess.run(["arm-vita-eabi-readelf", "-SW", ELF],
                                  capture_output=True, text=True, check=True).stdout
        symbols = subprocess.run(["arm-vita-eabi-nm", "-n", ELF],
                                 capture_output=True, text=True, check=True).stdout
    except (subprocess.CalledProcessError, FileNotFoundError) as exc:
        print("cannot read %s: %s" % (ELF, exc))
        return 2

    ranges = []
    for line in sections.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\.vitalink\.fstubs\.\S+)\s+\S+\s+"
                     r"([0-9a-f]+)\s+[0-9a-f]+\s+([0-9a-f]+)", line)
        if m:
            ranges.append((m.group(1), int(m.group(2), 16), int(m.group(3), 16)))

    syms = []
    for line in symbols.splitlines():
        parts = line.split()
        if len(parts) >= 3 and parts[1] in ("T", "t"):
            try:
                syms.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass

    suspicious = []
    for name, base, size in ranges:
        for addr, sym in syms:
            if not base <= addr < base + size:
                continue
            if sym.lower().startswith(OK_PREFIXES):
                continue
            suspicious.append((name, sym, addr))

    print("%s: %d stub section(s)" % (ELF, len(ranges)))
    if not suspicious:
        print("OK - every symbol in a stub section belongs to a Sce library")
        return 0

    print("SUSPICIOUS - these will execute address 0 at first call:")
    for section, sym, addr in suspicious:
        print("  %-40s %-30s 0x%08x" % (section, sym, addr))
    return 1


if __name__ == "__main__":
    sys.exit(main())
