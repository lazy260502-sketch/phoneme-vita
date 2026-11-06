#!/usr/bin/env python3
"""gen_charmap.py - generate vita_charmap.h for the Vita i18n converter.

Why this exists
---------------
phoneME Feature ships no generic charset converter: i18n_port's
`getLcGenConvMethods()` is the upstream "default" stub that returns NULL
(phoneme-midp/src/i18n/i18n_port/default/native/defaultGenConv.c), so
`new InputStreamReader(in, "SJIS")` and `String.getBytes("GBK")` throw
UnsupportedEncodingException on this port.  The run-time side of that is
implemented in
    phoneme-midp/src/i18n/i18n_port/vita/native/vitaGenConv.c
which needs byte<->Unicode tables for the encodings J2ME games actually
ship with (Shift_JIS, EUC-JP, EUC-KR, GBK/GB2312, Big5).

The tables are generated here rather than hand-written so they can be
regenerated (and re-verified against the host Python codecs) at any time:

    python3 tools/charsetgen/gen_charmap.py            # write the header
    python3 tools/charsetgen/gen_charmap.py --check    # verify, write nothing

Output goes to
    phoneme-midp/src/i18n/i18n_port/vita/native/vita_charmap.h

Layout per encoding (all tables are static const, so they land in .rodata):
    sb[256]        single byte -> Unicode (0 = no mapping)
    hi[256]        lead byte -> 1 + block index into db (0 = not a lead)
    db[nblk][256]  (lead, trail) -> Unicode (0 = no mapping)
    rev[n]         sorted keys (cp << 16) | code, where code is 0x00XX for a
                   one-byte sequence and 0xXXXX for a two-byte one; binary
                   search is used for Unicode -> native

Every table is static const so it lands in .rodata; nothing is allocated on
the Java heap.
"""

import argparse
import os
import sys

# (tag, python codec, aliases the Java layer may ask for)
ENCODINGS = [
    ("sjis", "shift_jis", ["SJIS", "Shift_JIS", "Shift-JIS", "MS932",
                           "Windows-31J", "x-sjis", "sjis"]),
    ("eucjp", "euc_jp", ["EUC-JP", "EUC_JP", "eucJP", "x-euc-jp"]),
    ("euckr", "euc_kr", ["EUC-KR", "EUC_KR", "eucKR", "x-euc-kr",
                         "KSC5601", "KS_C_5601-1987"]),
    ("gbk", "gbk", ["GBK", "GB2312", "GB_2312-80", "EUC-CN", "x-gbk",
                    "CP936", "gb2312"]),
    ("big5", "big5", ["Big5", "BIG5", "big5", "x-big5", "CP950"]),
]

OUT_REL = os.path.join("phoneme-midp", "src", "i18n", "i18n_port", "vita",
                       "native", "vita_charmap.h")


def build_tables(codec):
    """Return (sb, db, rev) for `codec`; see the module docstring."""
    sb = [0] * 256
    for b in range(256):
        try:
            s = bytes([b]).decode(codec)
        except (UnicodeDecodeError, LookupError):
            continue
        if len(s) == 1:
            sb[b] = ord(s)

    db = {}
    for b1 in range(0x80, 0x100):
        for b2 in range(0x100):
            try:
                s = bytes([b1, b2]).decode(codec)
            except (UnicodeDecodeError, LookupError):
                continue
            if len(s) != 1:
                continue
            cp = ord(s)
            if cp == 0:
                continue
            db[(b1, b2)] = cp

    # Unicode -> code.  Two-byte mappings first (more specific), then the
    # single-byte ones for code points no two-byte sequence covers.
    # Some codecs map one code point at several positions (Big5 duplicate
    # ideographs, e.g. U+5341 at A451 and A2CC); the *encoder* is the
    # authority there, so ask it and keep that answer.
    rev = {}
    for (b1, b2), cp in db.items():
        rev.setdefault(cp, (b1 << 8) | b2)
    for b, cp in enumerate(sb):
        if cp:
            rev.setdefault(cp, b)

    for cp in list(rev):
        try:
            enc = chr(cp).encode(codec)
        except (UnicodeEncodeError, LookupError):
            continue
        if len(enc) == 1:
            rev[cp] = enc[0]
        elif len(enc) == 2:
            rev[cp] = (enc[0] << 8) | enc[1]

    return sb, db, rev


def emit(w, name, sb, db, rev):
    """Write the four tables for one encoding through the `w(s)` writer."""
    leads = sorted({b1 for (b1, _b2) in db})
    hi = [0] * 256
    for i, b1 in enumerate(leads):
        hi[b1] = i + 1

    w("static const unsigned short %s_sb[256] = {\n" % name)
    for i in range(0, 256, 12):
        w("    " + ", ".join("0x%04X" % v for v in sb[i:i + 12]) + ",\n")
    w("};\n\n")

    w("static const unsigned short %s_hi[256] = {\n" % name)
    for i in range(0, 256, 12):
        w("    " + ", ".join("%d" % v for v in hi[i:i + 12]) + ",\n")
    w("};\n\n")

    n = max(1, len(leads) * 256)
    w("static const unsigned short %s_db[%d] = {\n" % (name, n))
    if leads:
        for b1 in leads:
            row = [db.get((b1, b2), 0) for b2 in range(256)]
            w("    /* 0x%02X */ " % b1 +
              ", ".join("0x%04X" % v for v in row) + ",\n")
    else:
        w("    0\n")
    w("};\n\n")

    keys = sorted((cp << 16) | code for cp, code in rev.items())
    w("static const unsigned int %s_rev[%d] = {\n" % (name, max(1, len(keys))))
    if keys:
        for i in range(0, len(keys), 6):
            w("    " + ", ".join("0x%08XU" % v for v in keys[i:i + 6]) + ",\n")
    else:
        w("    0U\n")
    w("};\n\n")


HEADER = """/*
 * vita_charmap.h - GENERATED by tools/charsetgen/gen_charmap.py. DO NOT EDIT.
 *
 * Byte<->Unicode tables for the Vita generic charset converter
 * (vitaGenConv.c).  Regenerate after changing the encoding list:
 *
 *     python3 tools/charsetgen/gen_charmap.py
 *
 * Every table is static const so it goes to .rodata; nothing here is
 * allocated on the Java heap.  See the generator for the layout.
 */
#ifndef VITA_CHARMAP_H
#define VITA_CHARMAP_H

"""

TAIL = """typedef struct {
    const char *const *names;
    const unsigned short *sb;
    const unsigned short *hi;
    const unsigned short *db;
    const unsigned int *rev;
    unsigned int rev_n;
} vita_charmap;

"""


def render(tables):
    out = [HEADER]
    for tag, _codec, aliases in ENCODINGS:
        sb, db, rev = tables[tag]
        out.append("/* ---- %s ---- */\n" % tag)
        out.append("static const char *const %s_names[] = {%s, 0};\n"
                   % (tag, ", ".join('"%s"' % a for a in aliases)))
        parts = []
        emit(parts.append, "m_" + tag, sb, db, rev)
        out.append("".join(parts))
    out.append(TAIL)
    rows = []
    for tag, _codec, _aliases in ENCODINGS:
        _sb, _db, rev = tables[tag]
        rows.append("    { %s_names, m_%s_sb, m_%s_hi, m_%s_db, m_%s_rev, %d },"
                    % (tag, tag, tag, tag, tag, len(rev)))
    out.append("static const vita_charmap vita_charmaps[] = {\n" +
               "\n".join(rows) + "\n};\n\n")
    out.append("#define VITA_CHARMAP_COUNT "
               "(int)(sizeof(vita_charmaps) / sizeof(vita_charmaps[0]))\n\n")
    out.append("#endif /* VITA_CHARMAP_H */\n")
    return "".join(out)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(os.path.dirname(here))

    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify the header is up to date, write nothing")
    ap.add_argument("-o", "--output", default=os.path.join(root, OUT_REL))
    args = ap.parse_args()

    tables = {}
    for tag, codec, _aliases in ENCODINGS:
        sb, db, rev = build_tables(codec)
        tables[tag] = (sb, db, rev)
        print("%-6s %-10s 1B=%3d 2B=%5d rev=%5d"
              % (tag, codec, sum(1 for v in sb if v), len(db), len(rev)),
              file=sys.stderr)

    data = render(tables)

    if args.check:
        try:
            with open(args.output, "r") as f:
                old = f.read()
        except OSError:
            old = None
        if old != data:
            print("OUT OF DATE: %s" % args.output, file=sys.stderr)
            return 1
        print("up to date: %s" % args.output, file=sys.stderr)
        return 0

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, "w") as f:
        f.write(data)
    print("wrote %s (%d bytes)" % (args.output, len(data)), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
