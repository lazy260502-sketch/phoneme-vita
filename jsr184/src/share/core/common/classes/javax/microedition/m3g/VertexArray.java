package javax.microedition.m3g;

/**
 * Per-vertex attribute array (positions, normals, colours, tex coords).
 * Values are widened to int at set() time so the renderer never has to
 * branch on the component type again.
 */
public class VertexArray extends Object3D {

    public static final int BYTE = 0;
    public static final int SHORT = 1;
    public static final int FIXED = 2;
    public static final int FLOAT = 3;

    int numVertices;
    int numComponents;
    int componentSize;
    int[] data;            /* scaled integers for BYTE/SHORT/FIXED */
    float[] floatData;     /* FLOAT path, kept raw */

    /** Loader path: values already decoded (see M3gReader). */
    VertexArray(int numVertices, int numComponents, int componentSize,
                int[] decoded) {
        this.numVertices = numVertices;
        this.numComponents = numComponents;
        this.componentSize = componentSize;
        this.data = decoded;
    }

    public VertexArray(int numVertices, int numComponents, int componentSize) {
        if (numVertices <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad vertex count"));
        }
        if (numComponents < 2 || numComponents > 4) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad component count"));
        }
        if (componentSize < 1 || componentSize > 4) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad component size"));
        }
        this.numVertices = numVertices;
        this.numComponents = numComponents;
        this.componentSize = componentSize;
        /* v03.30: one int per COMPONENT, i.e. data[v*numComponents + c] -
         * the layout M3gReader.readVertexArray produces and the one the
         * JSR-184 set()/get() contract uses (count * numComponents values).
         * This used to allocate (and index) numVertices*numComponents*
         * componentSize, so for SHORT data (componentSize 2) every vertex
         * after the first read at twice the right offset, and a game doing
         * `new VertexArray(n,3,2); set(0,n,short[3n])` ran off the end of
         * its own array (AIOOBE swallowed by the title's catch). */
        this.data = new int[numVertices * numComponents];
    }

    public VertexArray(int numVertices, int numComponents, int componentSize,
                       byte[] values) {
        this(numVertices, numComponents, componentSize);
        set(0, numVertices, values);
    }

    public VertexArray(int numVertices, int numComponents, int componentSize,
                       short[] values) {
        this(numVertices, numComponents, componentSize);
        set(0, numVertices, values);
    }

    public void set(int firstVertex, int numVertices, byte[] values) {
        copy(values, firstVertex, numVertices, false, false);
    }

    public void set(int firstVertex, int numVertices, short[] values) {
        copyShort(values, firstVertex, numVertices);
    }

    public void set(int firstVertex, int numVertices, float[] values) {
        if (values == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int n = numVertices * numComponents;
        if (floatData == null) {
            floatData = new float[this.numVertices * numComponents];
        }
        System.arraycopy(values, 0, floatData, firstVertex * numComponents, n);
    }

    private void copy(byte[] src, int firstVertex, int numVertices,
                      boolean unused1, boolean unused2) {
        if (src == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int stride = numComponents;
        int off = firstVertex * stride;
        int n = numVertices * stride;
        for (int i = 0; i < n; i++) {
            data[off + i] = src[i];
        }
    }

    private void copyShort(short[] src, int firstVertex, int numVertices) {
        if (src == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int stride = numComponents;
        int off = firstVertex * stride;
        int n = numVertices * stride;
        for (int i = 0; i < n; i++) {
            data[off + i] = src[i];
        }
    }

    public void get(int firstVertex, int numVertices, byte[] out) {
        int stride = numComponents;
        int off = firstVertex * stride;
        for (int i = 0; i < numVertices * stride; i++) {
            out[i] = (byte)data[off + i];
        }
    }

    /** Normalised value of component c of vertex v, in [-1,1] or [0,1]. */
    float component(int v, int c) {
        int idx = v * numComponents + c;
        if (floatData != null) {
            return floatData[idx];
        }
        switch (componentSize) {
        case 1: return data[idx] / 128.0f;
        case 2: return data[idx] / 32768.0f;
        default: return data[idx] / 65536.0f;
        }
    }

    /**
     * The raw component value, WITHOUT the byte/short normalisation above.
     * <p>
     * JSR-184 normalises byte/short only for COLOR and NORMAL. POSITION and
     * TEXCOORD keep their integer value and are converted by the scale/bias
     * stored in the VertexBuffer ("value = raw * scale + bias").
     * <p>
     * Using component() for positions was catastrophic: the raw integer was
     * divided by 32768 first and the scale is itself of order 1/32768, so the
     * variation between vertices was divided away and every vertex collapsed
     * onto the bias. Measured on 生化危机's hunter.m3g (SkinnedMesh,
     * scale=0.0276, bias=(0,628.1,95.7)): the first three vertices came out as
     * (-0.005, 628.112, 95.675), (-0.004, 628.111, 95.673), (-0.004,
     * 628.112, 95.675) - i.e. the whole model became a single point, which is
     * exactly "the characters and the scene are not visible". With the raw
     * value they are (-170, 1029, 6.4), (-142, 986, -52), (-140, 1041, 6.4).
     */
    float componentRaw(int v, int c) {
        int idx = v * numComponents + c;
        if (floatData != null) {
            return floatData[idx];
        }
        return data[idx];
    }
}
