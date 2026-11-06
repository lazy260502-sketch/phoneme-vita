package javax.microedition.m3g;

/**
 * Triangle strips given either implicitly (firstIndex + strip lengths) or
 * with an explicit index array.
 */
public class TriangleStripArray extends IndexBuffer {

    int[] strips;   /* strip lengths */
    int[] indices;  /* explicit, or generated sequentially */
    int[] tris = null;
    int triCount = 0;

    TriangleStripArray() {
    }

    public TriangleStripArray(int firstIndex, int[] stripLengths) {
        if (firstIndex < 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative first index"));
        }
        if (stripLengths == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        strips = new int[stripLengths.length];
        int total = 0;
        for (int i = 0; i < stripLengths.length; i++) {
            if (stripLengths[i] < 3) {
                throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("strip shorter than 3"));
            }
            strips[i] = stripLengths[i];
            total += stripLengths[i];
        }
        if (stripLengths.length == 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("no strips"));
        }
        indices = new int[total];
        for (int i = 0; i < total; i++) {
            indices[i] = firstIndex + i;
        }
    }

    public TriangleStripArray(int[] indices, int[] stripLengths) {
        if (indices == null || stripLengths == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        strips = new int[stripLengths.length];
        int total = 0;
        for (int i = 0; i < stripLengths.length; i++) {
            if (stripLengths[i] < 3) {
                throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("strip shorter than 3"));
            }
            strips[i] = stripLengths[i];
            total += stripLengths[i];
        }
        if (total != indices.length) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("strip lengths mismatch"));
        }
        this.indices = new int[indices.length];
        System.arraycopy(indices, 0, this.indices, 0, indices.length);
    }

    public void getIndices(int[] out) {
        System.arraycopy(indices, 0, out, 0, indices.length);
    }

    public int getIndexCount() {
        return indices.length;
    }

    /** Fan-triangulate each strip, flipping the winding on odd triangles. */
    int[] triangleList() {
        if (tris == null) {
            int n = 0;
            for (int i = 0; i < strips.length; i++) {
                n += (strips[i] - 2) * 3;
            }
            tris = new int[n];
            int o = 0, base = 0;
            for (int s = 0; s < strips.length; s++) {
                int len = strips[s];
                for (int k = 0; k + 2 < len; k++) {
                    if ((k & 1) == 0) {
                        tris[o++] = indices[base + k];
                        tris[o++] = indices[base + k + 1];
                        tris[o++] = indices[base + k + 2];
                    } else {
                        tris[o++] = indices[base + k + 1];
                        tris[o++] = indices[base + k];
                        tris[o++] = indices[base + k + 2];
                    }
                }
                base += len;
            }
            triCount = n;
        }
        return tris;
    }

    void collectReferences(java.util.Vector out) {
    }

    int triangleCount() {
        triangleList();
        return triCount / 3;
    }
}
