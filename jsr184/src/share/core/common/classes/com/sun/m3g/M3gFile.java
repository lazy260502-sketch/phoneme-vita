package com.sun.m3g;

import java.io.IOException;

/**
 * M3G file container parser.
 *
 * Splits a .m3g file into its object table without building the object
 * graph: every entry keeps the byte range of the object body, so the
 * per-type deserialiser (LoadTask) can read fields lazily.
 *
 * Layout (verified against the 15 .m3g files shipped with the CAPCOM title
 * in debug/3d_game):
 *   12 bytes  identifier  AB 'J' 'S' 'R' '1' '8' '4' BB 0D 0A 1A 0A
 *   section:  u8 compressionScheme
 *             u32 LE totalSectionLength   (header + payload + checksum)
 *             u32 LE uncompressedLength
 *             payload
 *             u32 LE checksum
 *   payload:  objects, each  u8 type, u32 LE bodyLength, body
 *   body:     u32 LE objectID, then the type-specific fields
 *   type 255: no objectID, the body is a UTF-8 resource name (xref)
 *
 * All integers are little-endian. Strings are NUL-terminated UTF-8.
 *
 * compressionScheme 1 (zlib) is not handled yet: none of the files in the
 * test set use it, and CLDC has no java.util.zip, so it needs either a
 * Java inflate or a native one (J2ME-Loader calls a native _inflate).
 */
public final class M3gFile {

    /** Object type codes from the JSR 184 file format.
     *
     * Verified two ways: against the 15 .m3g files in debug/3d_game (each
     * holds exactly one type 22 = World, one type 21 = VertexBuffer
     * referencing three type 20 = VertexArray objects, and one type 5 =
     * Camera), and against the object dispatch table of an independent
     * open-source M3G loader. */
    public static final int T_HEADER = 0;
    public static final int T_ANIMATION_CONTROLLER = 1;
    public static final int T_ANIMATION_TRACK = 2;
    public static final int T_APPEARANCE = 3;
    public static final int T_BACKGROUND = 4;
    public static final int T_CAMERA = 5;
    public static final int T_COMPOSITING_MODE = 6;
    public static final int T_FOG = 7;
    public static final int T_POLYGON_MODE = 8;
    public static final int T_GROUP = 9;
    public static final int T_IMAGE2D = 10;
    public static final int T_TRIANGLE_STRIP_ARRAY = 11;
    public static final int T_LIGHT = 12;
    public static final int T_MATERIAL = 13;
    public static final int T_MESH = 14;
    public static final int T_MORPHING_MESH = 15;
    public static final int T_SKINNED_MESH = 16;
    public static final int T_TEXTURE2D = 17;
    public static final int T_SPRITE3D = 18;
    public static final int T_KEYFRAME_SEQUENCE = 19;
    public static final int T_VERTEX_ARRAY = 20;
    public static final int T_VERTEX_BUFFER = 21;
    public static final int T_WORLD = 22;
    public static final int T_EXTERNAL_REF = 255;

    private static final byte[] IDENTIFIER = {
        (byte)0xAB, 'J', 'S', 'R', '1', '8', '4', (byte)0xBB, 13, 10, 26, 10
    };

    /** One object slot; body is data[bodyOffset .. bodyOffset+bodyLength). */
    public static final class Obj {
        public int type;
        public int id;          /* 0 for T_EXTERNAL_REF */
        public int bodyOffset;  /* start of the body (after the objectID) */
        public int bodyLength;
        public String externalName; /* type 255 only: referenced resource */

        public String toString() {
            return typeName(type) + "#" + id + "[" + bodyLength + "]";
        }
    }

    public int versionMajor;
    public int versionMinor;
    public boolean hasExternalRefs;
    public int totalFileSize;
    public String author;
    public int compression;      /* scheme of the first (header) section */
    public Obj[] objects;
    public String[] externalRefs;

    private M3gFile() {
    }

    public static boolean hasIdentifier(byte[] data, int offset) {
        if (data == null || offset < 0 || data.length - offset < IDENTIFIER.length) {
            return false;
        }
        for (int i = 0; i < IDENTIFIER.length; i++) {
            if (data[offset + i] != IDENTIFIER[i]) {
                return false;
            }
        }
        return true;
    }

    public static M3gFile parse(byte[] data, int offset) throws IOException {
        if (!hasIdentifier(data, offset)) {
            throw new IOException("not an M3G file");
        }
        Reader r = new Reader(data, offset + IDENTIFIER.length, data.length);
        M3gFile f = new M3gFile();
        f._data = data;
        java.util.Vector objs = new java.util.Vector();
        java.util.Vector xrefs = new java.util.Vector();

        /* ---- header section ---- */
        f.compression = r.u8();
        int totalLen = r.u32();
        int uncompLen = r.u32();
        if (f.compression != 0) {
            throw new IOException("M3G compression scheme "
                                  + f.compression + " not supported");
        }
        if (uncompLen != totalLen - 13) {
            throw new IOException("header length mismatch");
        }
        int sectionEnd = r.pos - 9 + totalLen;
        int headerType = r.u8();
        if (headerType != T_HEADER) {
            throw new IOException("bad header object type " + headerType);
        }
        int headerLen = r.u32();
        f.versionMajor = r.u8();
        f.versionMinor = r.u8();
        f.hasExternalRefs = r.u8() != 0;
        f.totalFileSize = r.u32();
        r.u32();                       /* approximate content size */
        f.author = r.string();
        /* The 4 bytes before the next section are the SECTION checksum, not
         * part of the header object (object length excludes them). */
        if (r.pos != sectionEnd - 4) {
            throw new IOException("header object length mismatch");
        }
        r.pos = sectionEnd;

        /* ---- remaining sections ---- */
        while (r.pos < r.limit) {
            int start = r.pos;
            int scheme = r.u8();
            int tot = r.u32();
            int unc = r.u32();
            if (tot <= 0 || start + tot > r.limit) {
                throw new IOException("bad section length " + tot);
            }
            if (scheme != 0) {
                throw new IOException("M3G compression scheme " + scheme
                                      + " not supported");
            }
            if (unc != tot - 13) {
                throw new IOException("section length mismatch");
            }
            int payloadEnd = start + tot - 4;
            while (r.pos < payloadEnd) {
                int type = r.u8();
                int len = r.u32();
                int body = r.pos;
                if (body + len > payloadEnd) {
                    throw new IOException("object overruns its section");
                }
                if (type == T_EXTERNAL_REF) {
                    /* External references occupy a slot in the object
                     * array: references are indices into that array, so
                     * dropping them would shift every following index. */
                    Obj x = new Obj();
                    x.type = T_EXTERNAL_REF;
                    x.id = 0;
                    x.bodyOffset = body;
                    x.bodyLength = len;
                    x.externalName = decodeString(data, body, body + len);
                    xrefs.addElement(x.externalName);
                    objs.addElement(x);
                    r.pos = body + len;
                    continue;
                }
                if (len < 4) {
                    throw new IOException("object too short for an id");
                }
                Obj o = new Obj();
                o.type = type;
                o.id = readU32(data, body);
                o.bodyOffset = body + 4;
                o.bodyLength = len - 4;
                objs.addElement(o);
                r.pos = body + len;
            }
            if (r.pos != payloadEnd) {
                throw new IOException("section payload mismatch");
            }
            r.pos = start + tot;       /* skip the checksum */
        }

        f.objects = new Obj[objs.size()];
        objs.copyInto(f.objects);
        f.externalRefs = new String[xrefs.size()];
        xrefs.copyInto(f.externalRefs);
        return f;
    }

    /** Body view for the per-type deserialiser. */
    public Reader body(Obj o) {
        return new Reader(_data, o.bodyOffset, o.bodyOffset + o.bodyLength);
    }

    /* Kept so body() can be called after parse; set at the end of parse. */
    private byte[] _data;

    public static String typeName(int t) {
        switch (t) {
        case T_HEADER: return "Header";
        case T_ANIMATION_CONTROLLER: return "AnimationController";
        case T_ANIMATION_TRACK: return "AnimationTrack";
        case T_APPEARANCE: return "Appearance";
        case T_BACKGROUND: return "Background";
        case T_CAMERA: return "Camera";
        case T_COMPOSITING_MODE: return "CompositingMode";
        case T_FOG: return "Fog";
        case T_POLYGON_MODE: return "PolygonMode";
        case T_GROUP: return "Group";
        case T_IMAGE2D: return "Image2D";
        case T_TRIANGLE_STRIP_ARRAY: return "TriangleStripArray";
        case T_LIGHT: return "Light";
        case T_MATERIAL: return "Material";
        case T_MESH: return "Mesh";
        case T_MORPHING_MESH: return "MorphingMesh";
        case T_SKINNED_MESH: return "SkinnedMesh";
        case T_TEXTURE2D: return "Texture2D";
        case T_SPRITE3D: return "Sprite3D";
        case T_KEYFRAME_SEQUENCE: return "KeyframeSequence";
        case T_VERTEX_ARRAY: return "VertexArray";
        case T_VERTEX_BUFFER: return "VertexBuffer";
        case T_WORLD: return "World";
        case T_EXTERNAL_REF: return "ExternalRef";
        default: return "type" + t;
        }
    }

    private static String decodeString(byte[] d, int from, int to) throws IOException {
        int end = from;
        while (end < to && d[end] != 0) {
            end++;
        }
        return utf8(d, from, end - from);
    }

    private static String utf8(byte[] d, int off, int len) throws IOException {
        StringBuffer sb = new StringBuffer(len);
        int i = off, end = off + len;
        while (i < end) {
            int c = d[i++] & 0xFF;
            if ((c & 0x80) == 0) {
                sb.append((char)c);
            } else if ((c & 0xE0) == 0xC0) {
                if (i >= end) {
                    throw new IOException("truncated UTF-8");
                }
                int c2 = d[i++] & 0xFF;
                sb.append((char)(((c & 0x1F) << 6) | (c2 & 0x3F)));
            } else if ((c & 0xF0) == 0xE0) {
                if (i + 1 >= end) {
                    throw new IOException("truncated UTF-8");
                }
                int c2 = d[i++] & 0xFF;
                int c3 = d[i++] & 0xFF;
                sb.append((char)(((c & 0x0F) << 12) | ((c2 & 0x3F) << 6)
                                 | (c3 & 0x3F)));
            } else {
                throw new IOException("bad UTF-8 lead byte");
            }
        }
        return sb.toString();
    }

    private static int readU32(byte[] d, int p) {
        return (d[p] & 0xFF) | ((d[p + 1] & 0xFF) << 8)
             | ((d[p + 2] & 0xFF) << 16) | ((d[p + 3] & 0xFF) << 24);
    }

    /** Little-endian cursor over a slice of the file. */
    public static final class Reader {
        private final byte[] d;
        private final int limit;
        int pos;

        public Reader(byte[] data, int from, int to) {
            d = data;
            pos = from;
            limit = to;
        }

        public int position() {
            return pos;
        }

        public int remaining() {
            return limit - pos;
        }

        public int u8() throws IOException {
            if (pos >= limit) {
                throw new IOException("unexpected end of M3G data");
            }
            return d[pos++] & 0xFF;
        }

        public boolean bool() throws IOException {
            int b = u8();
            if (b > 1) {
                throw new IOException("malformed boolean " + b);
            }
            return b != 0;
        }

        public int u32() throws IOException {
            if (limit - pos < 4) {
                throw new IOException("unexpected end of M3G data");
            }
            int v = readU32(d, pos);
            pos += 4;
            return v;
        }

        public float f32() throws IOException {
            return Float.intBitsToFloat(u32());
        }

        public String string() throws IOException {
            int start = pos;
            while (pos < limit && d[pos] != 0) {
                pos++;
            }
            if (pos >= limit) {
                throw new IOException("unterminated string");
            }
            String s = utf8(d, start, pos - start);
            pos++;                     /* skip NUL */
            return s;
        }

        /** Reads n bytes into a fresh array. */
        public byte[] bytes(int n) throws IOException {
            if (n < 0 || limit - pos < n) {
                throw new IOException("short read of " + n + " bytes");
            }
            byte[] r = new byte[n];
            System.arraycopy(d, pos, r, 0, n);
            pos += n;
            return r;
        }

        public int[] u32s(int n) throws IOException {
            int[] r = new int[n];
            for (int i = 0; i < n; i++) {
                r[i] = u32();
            }
            return r;
        }

        public float[] f32s(int n) throws IOException {
            float[] r = new float[n];
            for (int i = 0; i < n; i++) {
                r[i] = f32();
            }
            return r;
        }

        public void skip(int n) throws IOException {
            if (n < 0 || limit - pos < n) {
                throw new IOException("cannot skip " + n);
            }
            pos += n;
        }
    }
}
