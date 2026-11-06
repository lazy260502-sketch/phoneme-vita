#!/usr/bin/env python3
"""verify_charsets.py - full-table check of the Vita charset converter.

Compiles tools/charsetgen/test_vitagenuconv.c together with the real
phoneme-midp/src/i18n/i18n_port/vita/native/vitaGenConv.c on the host and
compares every mapping against Python's own codecs, in both directions.

    python3 tools/charsetgen/verify_charsets.py            # all encodings
    python3 tools/charsetgen/verify_charsets.py GBK SJIS   # a subset

Exit code 0 means "no mismatch".  A non-zero exit prints the first few
offending samples so the generator or the converter can be fixed.
"""

import os
import subprocess
import sys
import tempfile

ENCODINGS = [
    ("SJIS", "shift_jis"),
    ("EUC-JP", "euc_jp"),
    ("EUC-KR", "euc_kr"),
    ("GBK", "gbk"),
    ("Big5", "big5"),
]

MAX_SHOW = 8


def host_cc():
    for cc in ("/usr/bin/gcc", "/usr/bin/cc"):
        if os.path.exists(cc):
            return cc
    return "cc"


def build(root, exe):
    cmd = [
        host_cc(), "-B/usr/bin", "-O1", "-w",
        "-I", os.path.join(root, "tools/charsetgen/hoststub"),
        "-I", os.path.join(root, "phoneme-midp/src/i18n/i18n_port/include"),
        "-I", os.path.join(root, "phoneme-midp/src/i18n/i18n_port/vita/native"),
        "-o", exe,
        os.path.join(root, "tools/charsetgen/test_vitagenuconv.c"),
        os.path.join(root, "phoneme-midp/src/i18n/i18n_port/vita/native/vitaGenConv.c"),
    ]
    subprocess.run(cmd, check=True)


def run(exe, mode, enc, lines):
    p = subprocess.run([exe, mode, enc], input="\n".join(lines) + "\n",
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("converter failed (%s %s): %s" % (mode, enc, p.stderr))
    return p.stdout.splitlines()


def table_samples(codec):
    """(hexbytes, codepoint) for every single and two-byte sequence."""
    out = []
    for b in range(1, 256):
        try:
            s = bytes([b]).decode(codec)
        except (UnicodeDecodeError, LookupError):
            continue
        if len(s) == 1:
            out.append(("%02X" % b, ord(s)))
    for b1 in range(0x80, 0x100):
        for b2 in range(0x100):
            try:
                s = bytes([b1, b2]).decode(codec)
            except (UnicodeDecodeError, LookupError):
                continue
            if len(s) == 1 and ord(s) != 0:
                out.append(("%02X%02X" % (b1, b2), ord(s)))
    return out


def check_dec(exe, enc, codec):
    samples = table_samples(codec)
    got = run(exe, "dec", enc, [h for h, _ in samples])
    bad = []
    for (h, cp), line in zip(samples, got):
        want = "U+%04X" % cp
        if line.strip() != want:
            bad.append((h, want, line.strip()))
    return len(samples), bad


def check_enc(exe, enc, codec):
    # One sample per code point; two-byte sequences win over one-byte ones
    # in the generator, which is exactly what Python's encoder produces for
    # these codecs (ASCII stays one byte).
    cps = {}
    for h, cp in table_samples(codec):
        cps.setdefault(cp, h)
    keys = sorted(cps)
    got = run(exe, "enc", enc, ["U+%04X" % c for c in keys])
    bad = []
    for cp, line in zip(keys, got):
        got_hex = line.strip().upper()
        # Compare against Python, including "not encodable -> ?".
        try:
            native = chr(cp).encode(codec)
        except (UnicodeEncodeError, LookupError):
            native = b""
        if native and native.hex().upper() != got_hex:
            bad.append(("U+%04X" % cp, native.hex().upper(), got_hex))
        elif not native and got_hex != "3F":
            bad.append(("U+%04X" % cp, "3F('?')", got_hex))
    return len(keys), bad


def main():
    root = os.path.dirname(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))))
    todo = [(e, c) for e, c in ENCODINGS if len(sys.argv) < 2 or e in sys.argv[1:]]
    if not todo:
        print("nothing selected", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory() as td:
        exe = os.path.join(td, "vgc")
        build(root, exe)
        failures = 0
        for enc, codec in todo:
            n1, bad1 = check_dec(exe, enc, codec)
            n2, bad2 = check_enc(exe, enc, codec)
            ok = not bad1 and not bad2
            print("%-7s decode %5d  encode %5d  %s"
                  % (enc, n1, n2, "OK" if ok else "MISMATCH"))
            for what, want, got in (bad1 + bad2)[:MAX_SHOW]:
                print("    %-10s want %-14s got %s" % (what, want, got))
            if not ok:
                failures += 1
        return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
