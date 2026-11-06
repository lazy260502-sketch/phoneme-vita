package javax.microedition.m3g;

import com.sun.m3g.Matrix4;

/**
 * Skinned mesh. The bone table read from a .m3g file deforms the vertices
 * on the CPU (captureBindPose / applySkin, v03.57). Meshes built through
 * addTransform() are stored but still drawn rigidly.
 */
public class SkinnedMesh extends Mesh {

    /* Skinning reference table as stored in the file: each entry binds a
     * bone to a vertex range with a weight. */
    Group skeleton = null;
    Node[] bones = null;
    int[] firstVertex = null;
    int[] vertexCount = null;
    int[] boneWeights = null;

    private Node[] chain = null;       /* skeleton nodes the bones hang under, parents before children */
    private int[] chainParent = null;  /* index into chain, -1 = top */
    private int[] entryChain = null;   /* bones[i] -> index into chain */
    private float[][] bindInv = null;  /* inverse bone-to-mesh at load time, per chain index; null = not a bone */
    private float[] scratchAcc = null;
    private float[] scratchW = null;

    SkinnedMesh(boolean placeholder) {
        super(true);
    }

    public SkinnedMesh(VertexBuffer vertexBuffer, IndexBuffer indexBuffer,
                       Appearance appearance) {
        super(vertexBuffer, indexBuffer, appearance);
    }

    public void addTransform(Node bone, int weight, int firstVertex,
                             int numVertices, int weightComponents) {
        if (bone == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        /* Skeleton data is accepted so models load; deformation is pending. */
    }

    void collectReferences(java.util.Vector out) {
        if (skeleton != null) out.addElement(skeleton);
        if (bones != null) {
            for (int i = 0; i < bones.length; i++) {
                if (bones[i] != null) out.addElement(bones[i]);
            }
        }
        super.collectReferences(out);
    }

    public void setSkeleton(Group skeleton) {
        this.skeleton = skeleton;
    }

    public Group getSkeleton() {
        return skeleton;
    }

    private int chainIndex(Node n, java.util.Vector nodes, java.util.Vector parents) {
        if (n == null || n == this) {
            return -1;
        }
        int at = nodes.indexOf(n);
        if (at >= 0) {
            return at;
        }
        int p = chainIndex(n.parent, nodes, parents);
        nodes.addElement(n);
        parents.addElement(new Integer(p));
        return nodes.size() - 1;
    }

    /* Node-to-mesh matrices. The skeleton root's own parent is null in a loaded file (it is not attached under the
     * mesh), so a chain ends at "no parent" and includes that root's local matrix. */
    private static void computeWorld(Node[] cn, int[] cp, float[][] w) {
        for (int k = 0; k < cn.length; k++) {
            float[] l = new float[16];
            cn[k].getCompositeMatrix(l);
            if (cp[k] < 0) {
                w[k] = l;
            } else {
                float[] o = new float[16];
                Matrix4.mul(o, w[cp[k]], l);
                w[k] = o;
            }
        }
    }

    /** Records the inverse of every bone's current bone-to-mesh matrix (the bind pose). Called once after loading. */
    void captureBindPose() {
        if (bones == null || bones.length == 0 || vertexBuffer == null) {
            return;
        }
        java.util.Vector nodes = new java.util.Vector();
        java.util.Vector parents = new java.util.Vector();
        int[] ec = new int[bones.length];
        for (int i = 0; i < bones.length; i++) {
            ec[i] = chainIndex(bones[i], nodes, parents);
        }
        int m = nodes.size();
        Node[] cn = new Node[m];
        int[] cp = new int[m];
        for (int k = 0; k < m; k++) {
            cn[k] = (Node)nodes.elementAt(k);
            cp[k] = ((Integer)parents.elementAt(k)).intValue();
        }
        float[][] w = new float[m][];
        computeWorld(cn, cp, w);
        float[][] inv = new float[m][];
        boolean[] tried = new boolean[m];
        int bonesOk = 0;
        int singular = 0;
        for (int i = 0; i < ec.length; i++) {
            int k = ec[i];
            if (k < 0 || tried[k]) {
                continue;
            }
            tried[k] = true;
            float[] iv = new float[16];
            if (Matrix4.invert(iv, w[k])) {
                inv[k] = iv;
                bonesOk++;
            } else {
                singular++;
            }
        }
        chain = cn;
        chainParent = cp;
        entryChain = ec;
        bindInv = inv;
        bonesReady = false;
        com.sun.m3g.M3gTrace.log("skin bind: entries=" + ec.length + " chain=" + m + " bones=" + bonesOk
            + " singular=" + singular);
    }

    /**
     * Deforms pos (n vertices, x y z, the mesh's own bind-pose coordinates) in place: each vertex becomes the
     * weight-normalised sum of (bone-now * bind-inverse) applied to it. Vertices no entry touches stay as they are.
     */
    /** v03.84: false makes applySkin use the Java loop (defaults.cfg m3gnative=0, or after a native refusal). */
    static boolean useNative = true;
    /** v04.24: false keeps the bone matrices in Java (set when the native bones() refuses, or with m3gnative=0). */
    static boolean useNativeBones = true;
    private float[] localFlat = null;
    private float[] bindFlat = null;
    private byte[] usedFlat = null;
    private boolean bonesReady = false;
    private float[] packMats = null;
    private int[] packEnt = null;
    /* v04.10: per-frame matrices, allocated once per chain length (log177: ~330 allocations per skinned mesh per frame) */
    private float[][] wBuf = null;
    private float[][] bmBuf = null;
    private byte[] stateBuf = null;
    private float[] localBuf = null;

    /** Same result as computeWorld, into preallocated matrices. */
    private void computeWorldInto(float[][] w) {
        float[] l = localBuf;
        for (int k = 0; k < chain.length; k++) {
            chain[k].getCompositeMatrix(l);
            if (chainParent[k] < 0) {
                System.arraycopy(l, 0, w[k], 0, 16);
            } else {
                Matrix4.mulNoAlias(w[k], w[chainParent[k]], l);
            }
        }
    }

    void applySkin(float[] pos, int n) {
        if (chain == null || entryChain == null || n <= 0) {
            return;
        }
        if (wBuf == null || wBuf.length != chain.length) {
            wBuf = new float[chain.length][16];
            bmBuf = new float[chain.length][16];
            stateBuf = new byte[chain.length];
            localBuf = new float[16];
        }
        float[][] bm = bmBuf;
        byte[] state = stateBuf;                   /* 0 not built, 1 usable, 2 non-finite */
        long t0;
        boolean haveBones = useNative && useNativeBones && bonesNative();   /* fills packMats and stateBuf */
        if (!haveBones) {
            t0 = System.currentTimeMillis();
            float[][] w = wBuf;
            computeWorldInto(w);
            Renderer.ptAdd(9, t0);
            t0 = System.currentTimeMillis();
            for (int k = 0; k < state.length; k++) {
                state[k] = 0;
            }
            for (int e = 0; e < entryChain.length; e++) {
                int k = entryChain[e];
                if (k < 0 || bindInv[k] == null || boneWeights[e] <= 0 || state[k] != 0) {
                    continue;
                }
                float[] mm = bm[k];
                Matrix4.mulNoAlias(mm, w[k], bindInv[k]);
                state[k] = 1;
                for (int q = 0; q < 16; q++) {
                    if (mm[q] != mm[q] || mm[q] > 1e20f || mm[q] < -1e20f) {
                        state[k] = 2;
                    }
                }
            }
            Renderer.ptAdd(10, t0);
        }
        t0 = System.currentTimeMillis();
        if (useNative) {
            boolean done;
            if (haveBones) {
                done = callSkin(pos, n, state);
                if (!done) {
                    for (int k = 0; k < state.length; k++) {      /* the Java loop below reads bm[] */
                        if (state[k] == 1) {
                            System.arraycopy(packMats, k * 16, bm[k], 0, 16);
                        }
                    }
                }
            } else {
                done = applySkinNative(pos, n, bm, state);
            }
            if (done) {
                Renderer.ptAdd(11, t0);
                return;
            }
        }
        if (scratchAcc == null || scratchAcc.length < n * 3) {
            scratchAcc = new float[n * 3];
            scratchW = new float[n];
        }
        float[] acc = scratchAcc;
        float[] ws = scratchW;
        for (int i = 0; i < n * 3; i++) {
            acc[i] = 0.0f;
        }
        for (int i = 0; i < n; i++) {
            ws[i] = 0.0f;
        }
        for (int e = 0; e < entryChain.length; e++) {
            int k = entryChain[e];
            int wt = boneWeights[e];
            if (k < 0 || bindInv[k] == null || wt <= 0) {
                continue;
            }
            if (state[k] != 1) {
                continue;
            }
            float[] m = bm[k];
            int v0 = firstVertex[e];
            int v1 = v0 + vertexCount[e];
            if (v0 < 0) {
                v0 = 0;
            }
            if (v1 > n) {
                v1 = n;
            }
            float fw = (float)wt;
            for (int v = v0; v < v1; v++) {
                float x = pos[v * 3];
                float y = pos[v * 3 + 1];
                float z = pos[v * 3 + 2];
                acc[v * 3] += fw * (m[0] * x + m[4] * y + m[8] * z + m[12]);
                acc[v * 3 + 1] += fw * (m[1] * x + m[5] * y + m[9] * z + m[13]);
                acc[v * 3 + 2] += fw * (m[2] * x + m[6] * y + m[10] * z + m[14]);
                ws[v] += fw;
            }
        }
        for (int v = 0; v < n; v++) {
            float s = ws[v];
            if (s > 0.0f) {
                float r = 1.0f / s;
                pos[v * 3] = acc[v * 3] * r;
                pos[v * 3 + 1] = acc[v * 3 + 1] * r;
                pos[v * 3 + 2] = acc[v * 3 + 2] * r;
            }
        }
        Renderer.ptAdd(11, t0);
    }

    private void ensurePack() {
        if (packMats == null || packMats.length != chain.length * 16) {
            packMats = new float[chain.length * 16];
        }
    }

    /**
     * v04.24: the local matrices go to native code (the composite matrix of a node is cached in Transformable), which
     * returns the bone-to-bind matrices straight into packMats and the per-bone state into stateBuf. false = refused.
     */
    private boolean bonesNative() {
        int nk = chain.length;
        if (!bonesReady) {
            localFlat = new float[nk * 16];
            bindFlat = new float[nk * 16];
            usedFlat = new byte[nk];
            for (int k = 0; k < nk; k++) {
                if (bindInv[k] != null) {
                    System.arraycopy(bindInv[k], 0, bindFlat, k * 16, 16);
                }
            }
            for (int e = 0; e < entryChain.length; e++) {
                int k = entryChain[e];
                if (k >= 0 && bindInv[k] != null && boneWeights[e] > 0) {
                    usedFlat[k] = 1;
                }
            }
            bonesReady = true;
        }
        ensurePack();
        long t0 = System.currentTimeMillis();
        float[] l = localBuf;
        for (int k = 0; k < nk; k++) {
            chain[k].getCompositeMatrix(l);
            System.arraycopy(l, 0, localFlat, k * 16, 16);
        }
        Renderer.ptAdd(9, t0);
        t0 = System.currentTimeMillis();
        boolean ok = false;
        try {
            ok = com.sun.m3g.NativeBridge.bones(localFlat, chainParent, bindFlat, usedFlat, nk, packMats, stateBuf);
        } catch (Throwable t) {
            ok = false;
        }
        Renderer.ptAdd(10, t0);
        if (!ok) {
            useNativeBones = false;
            com.sun.m3g.M3gTrace.log("native bones refused; using the Java loop");
        }
        return ok;
    }

    /** Packs the usable bone matrices and the entry table and lets the native kernel do the per-vertex work. */
    private boolean applySkinNative(float[] pos, int n, float[][] bm, byte[] state) {
        ensurePack();
        for (int k = 0; k < chain.length; k++) {
            if (state[k] == 1) {
                System.arraycopy(bm[k], 0, packMats, k * 16, 16);
            }
        }
        return callSkin(pos, n, state);
    }

    /** packMats already holds the usable matrices; build the entry table and run the kernel. */
    private boolean callSkin(float[] pos, int n, byte[] state) {
        int ne = entryChain.length;
        if (packEnt == null || packEnt.length != ne * 4) {
            packEnt = new int[ne * 4];
        }
        for (int e = 0; e < ne; e++) {
            int k = entryChain[e];
            boolean ok = k >= 0 && bindInv[k] != null && boneWeights[e] > 0 && state[k] == 1;
            packEnt[e * 4] = ok ? k : -1;
            packEnt[e * 4 + 1] = firstVertex[e];
            packEnt[e * 4 + 2] = vertexCount[e];
            packEnt[e * 4 + 3] = boneWeights[e];
        }
        boolean done = false;
        try {
            done = com.sun.m3g.NativeBridge.skin(pos, n, packMats, packEnt, ne);
        } catch (Throwable t) {
            done = false;
        }
        if (!done) {
            useNative = false;
            com.sun.m3g.M3gTrace.log("native skin refused; using the Java loop");
        }
        return done;
    }
}
