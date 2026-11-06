#!/usr/bin/env python3
"""Slim vita-port/CMakeLists.txt: drop the historical per-release comment blocks.

Why: every release appended a `set(VITA_VERSION "xx.yy")` preceded by a long
comment essay. There are 180 of them - 5491 of 7256 lines (76%) - and only the
LAST one does anything, because CMake is last-set-wins. The essays are kept in
PROJECT_MEMORY.md (searchable by version); leaving them in the build file just
buries the actual configuration (537 functional lines).

What it does: for every block (a run of comment lines immediately followed by
`set(VITA_VERSION ...)`), delete that run; at the position of the LAST block
leave a short marker, so the version is still defined before the later
`${VITA_VERSION}` uses.

Safety: --write refuses unless the functional lines (non-comment, non-blank) are
byte-identical before and after, and unless the marker ends up as the last
set(VITA_VERSION). Default is a dry run.

Usage:
    python3 tools/slim_cmake.py            # report only
    python3 tools/slim_cmake.py --write    # apply
"""
import sys

PATH = 'vita-port/CMakeLists.txt'
MARKER = [
    '# ---- version marker ---------------------------------------------------',
    '# Every release used to append its own essay + set(VITA_VERSION) above this',
    '# point: 180 of them, and only the last one had any effect (CMake is',
    '# last-set-wins). Those essays are preserved in PROJECT_MEMORY.md, findable',
    '# by version number; keeping them here buried the real configuration.',
    '# Keep this as the LAST set(VITA_VERSION) in the file.',
    'set(VITA_VERSION  "04.54")',
]


def version_blocks(lines):
    """[(start, set_line_index)] for every `set(VITA_VERSION ...)` + its comments."""
    out = []
    for i, l in enumerate(lines):
        if 'set(VITA_VERSION' in l:
            s = i
            while s > 0 and lines[s - 1].strip().startswith('#'):
                s -= 1
            out.append((s, i))
    return out


def functional(seq):
    # Exclude the version marker itself: 180 of them collapse into 1, so they
    # would always "mismatch". Everything else must survive byte for byte.
    return [l.rstrip() for l in seq
            if l.strip() and not l.strip().startswith('#')
            and 'set(VITA_VERSION' not in l]


def main():
    write = '--write' in sys.argv
    lines = open(PATH, encoding='utf-8', errors='replace').read().splitlines()
    bl = version_blocks(lines)
    if not bl:
        print('no version blocks found - nothing to do')
        return 1

    # every line inside a block except the set() must be a comment, otherwise
    # this tool would silently delete configuration
    bad = []
    drop = set()
    for (s, e) in bl:
        for i in range(s, e):
            t = lines[i].strip()
            if t and not t.startswith('#'):
                bad.append((i + 1, t[:70]))
        for i in range(s, e + 1):
            drop.add(i)
    if bad:
        print('REFUSING: %d line(s) inside version blocks are not comments:' % len(bad))
        for ln, t in bad[:10]:
            print('  line %d: %s' % (ln, t))
        return 2

    keep_at = bl[-1][0]
    new = []
    for i, l in enumerate(lines):
        if i == keep_at:
            new.extend(MARKER)
        if i in drop:
            continue
        new.append(l)

    # deletions leave runs of blank lines behind
    out = []
    for l in new:
        if not l.strip() and out and not out[-1].strip():
            continue
        out.append(l)

    a, b = functional(lines), functional(out)
    print('lines:            %d -> %d  (-%d)' % (len(lines), len(out), len(lines) - len(out)))
    print('version blocks:   %d removed' % len(bl))
    print('functional lines: %d -> %d' % (len(a), len(b)))
    if a != b:
        n = min(len(a), len(b))
        for i in range(n):
            if a[i] != b[i]:
                print('MISMATCH at functional line %d\n  old: %s\n  new: %s' % (i + 1, a[i], b[i]))
                break
        else:
            print('MISMATCH in count (old %d, new %d)' % (len(a), len(b)))
        print('REFUSING to write')
        return 3
    print('functional lines identical: OK')

    sets = [i for i, l in enumerate(out)
            if 'set(VITA_VERSION' in l and not l.strip().startswith('#')]
    print('set(VITA_VERSION left: %d, at line %d of %d' % (len(sets), sets[-1] + 1, len(out)))
    if len(sets) != 1:
        print('REFUSING: expected exactly one set(VITA_VERSION); leftovers:')
        for i in sets:
            print('  line %d: %s' % (i + 1, out[i][:90]))
        return 4

    if not write:
        print('dry run - pass --write to apply')
        return 0
    open(PATH, 'w', encoding='utf-8').write('\n'.join(out) + '\n')
    print('written: %s' % PATH)
    return 0


if __name__ == '__main__':
    sys.exit(main())
