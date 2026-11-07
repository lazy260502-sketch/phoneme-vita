#!/usr/bin/env python3
"""mbacinspect - read and sanity-check MBAC (Mascot Capsule Micro3D model) files.

Why this exists: the Mascot Capsule V3 container format ("MBAC") is not published
by Aplix. Our port only stored the bytes and drew a placeholder (see
vita-port/jsr_src/com/vodafone/v10/graphics/j3d/Figure.java). J2ME Loader's
ru/woesss/j2me/micro3d/Loader.java is an Apache-2.0 implementation of the format,
and this script re-implements its header parse in Python so the layout can be
verified against real game assets BEFORE any Java is written.

Header layout (from Loader.loadMbacData, verified against
debug/3d_game2/fly_and_destroy_3d/.../m_arrow_aback.mbac - 602 bytes):

    0  'M' 'B'
    2  version        u8   2..5
    3  0              u8   must be 0
   >3  4  vertexFmt   u8   1 = V1, 2 = V2
    5  normalFmt     u8   0 = none, 1 = V1, 2 = V2
    6  polygonFmt    u8   < 3 means "single texture, no C3/C4 counts"
    7  boneFmt       u8   must be 1
    8  numVertices   u16  <= 21845
   10  numPolyT3     u16  triangles
   12  numPolyT4     u16  quads
   14  numBones      u16
   (polygonFmt >= 3 only, from 16)
   16  numPolyC3     u16
   18  numPolyC4     u16
   20  numTextures   u16  <= 16
   22  numPatterns   u16  <= 33
   24  numColors     u16  <= 256
   then, for version 5 only: numPatterns * (numTextures + 1) * 2 u16

Usage:
    python3 tools/mbacinspect/mbacinspect.py <file.mbac|game.jar> [...]
"""
import sys
import zipfile


SYNTAX_ERR = 'MBAC syntax error'


def u16(b, i):
    if i + 1 >= len(b):
        raise ValueError('%s: truncated at %d' % (SYNTAX_ERR, i))
    return b[i] | (b[i + 1] << 8)


class Bits(object):
    """LSB-first bit reader, same arithmetic as Loader.readUBits/readBits.

    Loader fills `cache` from the low end (`cache |= byte << cached`) and takes
    the low `size` bits, so the first bit of a byte is the least significant one.
    """

    def __init__(self, data, pos):
        self.data = data
        self.pos = pos
        self.cache = 0
        self.cached = 0

    def ubits(self, size):
        while size > self.cached:
            if self.pos >= len(self.data):
                raise ValueError('bit stream truncated')
            self.cache |= self.data[self.pos] << self.cached
            self.pos += 1
            self.cached += 8
        result = self.cache & ((1 << size) - 1)
        self.cached -= size
        self.cache >>= size
        return result

    def bits(self, size):
        v = self.ubits(size)
        if v & (1 << (size - 1)):
            v -= (1 << size)
        return v


VERTEX_SIZES = (8, 10, 13, 16)


def read_vertices(data, model):
    """Decode `numVertices` xyz triples (vertexFormat 2 = the V2 bit stream)."""
    b = Bits(data, model['header_end'])
    want = model['vertices']
    out = []
    while len(out) < want:
        chunk = b.ubits(8)
        size = VERTEX_SIZES[chunk >> 6]
        count = (chunk & 0x3F) + 1
        if count > want - len(out):
            raise ValueError('vertex chunk overruns: %d > %d'
                             % (count, want - len(out)))
        for _ in range(count):
            out.append((b.bits(size), b.bits(size), b.bits(size)))
    return out, b.pos


def parse(data, name='?'):
    if len(data) < 26 or data[0] != 0x4D or data[1] != 0x42:
        raise ValueError('%s: not MBAC (magic %s)' % (name, data[:2].hex()))
    version = data[2]
    if data[3] != 0:
        raise ValueError('%s: byte 3 is %d, expected 0' % (name, data[3]))
    if not (2 <= version <= 5):
        raise ValueError('%s: unsupported version %d' % (name, version))

    if version > 3:
        vertex_fmt, normal_fmt, polygon_fmt, bone_fmt = (
            data[4], data[5], data[6], data[7])
        off = 8
    else:
        vertex_fmt, normal_fmt, polygon_fmt, bone_fmt = 1, 0, 1, 1
        off = 4
    if bone_fmt != 1:
        raise ValueError('%s: boneFmt %d (expected 1)' % (name, bone_fmt))

    num_vertices = u16(data, off)
    num_t3 = u16(data, off + 2)
    num_t4 = u16(data, off + 4)
    num_bones = u16(data, off + 6)
    off += 8

    if polygon_fmt < 3:
        num_c3 = num_c4 = num_colors = 0
        num_textures = num_patterns = 1
    else:
        num_c3 = u16(data, off)
        num_c4 = u16(data, off + 2)
        num_textures = u16(data, off + 4)
        num_patterns = u16(data, off + 6)
        num_colors = u16(data, off + 8)
        off += 10

    if num_vertices > 21845 or num_textures > 16 or num_patterns > 33 \
            or num_colors > 256:
        raise ValueError('%s: counts out of range v=%d tex=%d pat=%d col=%d'
                         % (name, num_vertices, num_textures, num_patterns,
                            num_colors))

    patterns = []
    if version == 5:
        for _ in range(num_patterns):
            row = []
            for _ in range(num_textures + 1):
                row.append((u16(data, off), u16(data, off + 2)))
                off += 4
            patterns.append(row)
    elif num_patterns:
        patterns = [[(num_c3, num_c4), (num_t3, num_t4)]]
        off += 0

    return {
        'name': name, 'bytes': len(data), 'version': version,
        'vertexFmt': vertex_fmt, 'normalFmt': normal_fmt,
        'polygonFmt': polygon_fmt, 'boneFmt': bone_fmt,
        'vertices': num_vertices, 'T3': num_t3, 'T4': num_t4,
        'bones': num_bones, 'C3': num_c3, 'C4': num_c4,
        'textures': num_textures, 'patterns': num_patterns,
        'colors': num_colors, 'header_end': off, 'pattern0': patterns[0] if patterns else None,
    }


def show(m):
    polys = m['T3'] + m['T4'] + m['C3'] + m['C4']
    print('%-34s %6dB  v%d fmt=%d/%d/%d  verts=%-4d poly=%-4d'
          ' (T3=%d T4=%d C3=%d C4=%d)  bones=%d tex=%d pat=%d col=%d'
          % (m['name'], m['bytes'], m['version'],
             m['vertexFmt'], m['normalFmt'], m['polygonFmt'],
             m['vertices'], polys, m['T3'], m['T4'], m['C3'], m['C4'],
             m['bones'], m['textures'], m['patterns'], m['colors']))


def from_path(path):
    out = []
    if path.lower().endswith('.jar'):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                if n.lower().endswith('.mbac'):
                    d = z.read(n)
                    out.append((parse(d, n.rsplit('/', 1)[-1]), d))
    else:
        with open(path, 'rb') as f:
            d = f.read()
        out.append((parse(d, path.rsplit('/', 1)[-1]), d))
    return out


def show_vertices(model, data):
    verts, end = read_vertices(data, model)
    xs = [v[0] for v in verts]
    ys = [v[1] for v in verts]
    zs = [v[2] for v in verts]
    print('    vertices: %d decoded, bit stream ends at %d of %d bytes'
          % (len(verts), end, len(data)))
    print('    x %6d..%-6d  y %6d..%-6d  z %6d..%-6d'
          % (min(xs), max(xs), min(ys), max(ys), min(zs), max(zs)))
    print('    first 4: %s' % (['(%d,%d,%d)' % v for v in verts[:4]],))


def main(argv):
    if not argv:
        print(__doc__)
        return 2
    want_verts = '--verts' in argv
    paths = [a for a in argv if not a.startswith('--')]
    if not paths:
        print(__doc__)
        return 2
    bad = 0
    for p in paths:
        try:
            models = from_path(p)
        except Exception as e:
            print('%-34s FAILED: %s' % (p, e))
            bad += 1
            continue
        for m, d in sorted(models, key=lambda x: -x[0]['vertices']):
            show(m)
            if want_verts:
                try:
                    show_vertices(m, d)
                except Exception as e:
                    print('    VERTEX DECODE FAILED: %s' % e)
                    bad += 1
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
