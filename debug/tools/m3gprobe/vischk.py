#!/usr/bin/env python3
"""Cross-check the JSR-184 API surface a title actually USES against the
visibility of the members we actually BUILT.

WHY THIS EXISTS
===============
Four separate bugs in this port had the same shape: the application makes a
legal API call and the port throws instead of doing the work. Each one froze a
title that looks like "nothing happens", because the throw lands inside a
midlet Timer task and TimerThread cancels the task silently (our [TIMEREXC]
probe is the only reason we ever saw them).

  v03.29  Texture2D.FUNC_BLEND (228) MISSING
          -> setBlending(228) threw IllegalArgumentException
  v04.17  PolygonMode.WINDING_CCW/CW were 176/177 (spec: 168/169)
          -> setWinding(169) threw IllegalArgumentException
  v04.17  Light.SPOT (131) MISSING -> setMode(131) threw
  v04.21  Appearance.setLayer(int) declared PACKAGE-PRIVATE (spec: public)
          -> invokevirtual failed the access check -> IllegalAccessError

The last one is invisible in a log: with USE_VERBOSE_ERROR_MSG off, ErrorMsgTag
is a plain enum index, so Throw::error() prints the NUMBER:
    java.lang.Error:  95        # 95 == illegal_access_error
Nothing in that line says "access", "visibility" or even which class. Hence
this checker: it does not care what we believe the spec says, it compares the
title's own constant pool against our compiled classes.

WHAT IT DOES
============
1. For every .class in the given jar, dump the constant pool with `javap -v`.
2. Keep every Methodref / InterfaceMethodref / Fieldref whose owner is under
   javax/microedition/m3g/ or com/sun/m3g/.
3. Resolve each one against the classpath with `javap -p -s` and report:
      MISSING  - we do not have that member at all (would be NoSuchMethodError)
      PACKAGE / PRIVATE / PROTECTED
               - we have it, but not public (would be IllegalAccessError)
      CLASS-MISSING
               - we do not have the class at all (NoClassDefFoundError)
Exit 0 only when every referenced member exists AND is public.

USAGE
=====
    python3 debug/tools/m3gprobe/vischk.py <classes-cp> <jar-or-dir> [...]

    # 极品飞车 against the built MIDP classes
    python3 debug/tools/m3gprobe/vischk.py \
        phoneme-midp/build/vita_arm/classes \
        "debug/3d_game/极品飞车13之变速英文3D版.jar"

    # every 3D sample at once
    python3 debug/tools/m3gprobe/vischk.py \
        phoneme-midp/build/vita_arm/classes debug/3d_game/*.jar

    # also worth running against the MIDP test suite / any 2D title
    python3 debug/tools/m3gprobe/vischk.py phoneme-midp/build/vita_arm/classes \
        "vita-port/midlets"/*.jar

Requires `javap` (any JDK; tested with OpenJDK 8).

NOTE ON CONSTRUCTORS: javap prints them as `ClassName(...)`, not `<init>(...)`,
so they are matched by name too. A <init> that is merely non-public only breaks
`new`, which the verifier catches at compile time for the title, so a
non-public <init> is reported but is not the usual culprit.
"""
import os
import re
import subprocess
import sys
import tempfile
import zipfile

M3G_PKGS = ('javax/microedition/m3g/', 'com/sun/m3g/')
# Capture the FULL owner path: an earlier version captured only the simple
# class name and then asked javap for a class literally called "Node", which
# reported every class as missing. Keep the package in group 1.
REF_RE = re.compile(
    r'//\s*((?:javax/microedition/m3g|com/sun/m3g)/[A-Za-z0-9_$/]+)\.'
    r'("[^"]+"|[A-Za-z0-9_$<>]+):(\S+)')


def jar_classes(path):
    """Yield (classfile_path, class_name) for a jar unpacked to a temp dir."""
    tmp = tempfile.mkdtemp(prefix='vischk')
    with zipfile.ZipFile(path) as z:
        z.extractall(tmp)
    for root, _dirs, files in os.walk(tmp):
        for f in files:
            if f.endswith('.class'):
                yield os.path.join(root, f)


def refs_of(classfile):
    p = subprocess.run(['javap', '-v', '-p', classfile],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return []
    return REF_RE.findall(p.stdout)


_member_cache = {}


def members(cls):
    """{name+descriptor: visibility} for one class, or None if absent."""
    if cls in _member_cache:
        return _member_cache[cls]
    p = subprocess.run(['javap', '-p', '-s', '-cp', CP, cls.replace('/', '.')],
                       capture_output=True, text=True)
    if p.returncode != 0:
        _member_cache[cls] = None
        return None
    out = {}
    pending = None
    simple = cls.rsplit('/', 1)[-1]
    VIS = r'(?:(public|protected|private) )?'
    for ln in p.stdout.splitlines():
        # Constructors are printed WITH their package as the only token before
        # the paren:   public javax.microedition.m3g.Appearance();
        # while ordinary methods have a return type first:
        #              public void setWinding(int);
        # The constructor's constant-pool name is <init>, so normalise it -
        # otherwise every `new Foo()` in a title looks like a missing method.
        mc = re.match(r'^  ' + VIS + r'(?:static )?(?:final )?'
                      r'([\w\.$]+)\((.*)$', ln)
        if mc and mc.group(2).rsplit('.', 1)[-1] == simple:
            pending = ('<init>', mc.group(1) or 'package')
            continue
        m = re.match(r'^  ' + VIS + r'(?:static )?(?:final )?'
                     r'[\w\.\[\]<>$]+ ([\w$<>]+)\((.*)$', ln)
        if m:
            pending = (m.group(2), m.group(1) or 'package')
            continue
        m2 = re.match(r'^\s+descriptor: (\S+)', ln)
        if m2 and pending:
            out[pending[0] + m2.group(1)] = pending[1]
            pending = None
    _member_cache[cls] = out
    return out


def check(cp, jar):
    problems = []
    seen = set()
    for cf in jar_classes(jar):
        for owner, name, desc in refs_of(cf):
            if name.startswith('"'):
                name = name.strip('"')
            key = (owner, name, desc)
            if key in seen:
                continue
            seen.add(key)
            cls = owner.replace('/', '.')
            table = members(owner)
            if table is None:
                problems.append(('CLASS-MISSING', cls, ''))
                continue
            vis = table.get(name + desc)
            if vis is None:
                problems.append(('MISSING', cls, name + desc))
                continue
            if vis != 'public':
                problems.append((vis.upper(), cls, name + desc))
    return sorted(set(problems))


def main(argv):
    global CP
    if len(argv) < 3:
        print(__doc__)
        return 2
    CP, targets = argv[1], argv[2:]
    total = 0
    jars = 0
    for t in targets:
        if not os.path.exists(t):
            print('SKIP (no such file): %s' % t)
            continue
        jars += 1
        print('== %s' % os.path.basename(t))
        probs = check(CP, t)
        if not probs:
            print('   OK - every referenced m3g member exists and is public')
        for kind, cls, member in probs:
            print('   %-14s %s%s' % (kind, cls, ('.' + member) if member else ''))
            total += 1
    print('\n%d jar(s), %d problem(s)' % (jars, total))
    return 0 if total == 0 else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
