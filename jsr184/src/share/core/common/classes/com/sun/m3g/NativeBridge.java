package com.sun.m3g;

/*
 * Thin KNI seam onto the Vita 3D backend (vita-port/src/vita_3d.c).
 *
 * Everything here is a 1:1 wrapper: no caching, no validation beyond what
 * the backend already does. Renderer owns the lifetimes.
 */
public final class NativeBridge {

    private NativeBridge() {}

    /** Configured backend (0 none, 1 hardware, 2 software) after init. */
    public static native int init(int width, int height);

    public static native int backend();
    /** defaults.cfg m3gcam=: 0 inv (default), 1 direct, 2 invt. */
    public static native int camMode();

    /** argb is 0xAARRGGBB; returns a handle >= 1, or 0 on failure. */
    public static native int textureCreate(int[] argb, int width, int height);
    public static native void textureFree(int handle);

    /* v03.24: clearColor == false means "leave the colour buffer alone", which
     * is what Background.setColorClearEnable(false) asks for; the depth buffer
     * is still cleared when clearDepth is true. */
    public static native void clear(int argb, boolean clearDepth,
                                   boolean clearColor);
    /** Restricts subsequent clears and draws to a pixel rectangle. */
    public static native void setScissor(int x, int y, int width, int height);

    /* v03.22: drawing surface size (the M3G target for a Graphics).
     * 0 means "unknown" - callers keep their own fallback. */
    public static native int screenWidth();
    public static native int screenHeight();
    public static native void setMvp(float[] m16ColumnMajor);

    /**
     * v04.02: the full M3G CompositingMode state, not just "blend on/off".
     *
     * {@code blendMode} is the raw javax.microedition.m3g value (64 ALPHA,
     * 65 ALPHA_ADD, 66 MODULATE, 67 MODULATE_X2, 68 REPLACE); anything else
     * is treated as REPLACE. The three flags and the threshold map 1:1 onto
     * CompositingMode's accessors. {@code blendColor} is 0x00RRGGBB and is
     * only meaningful for the MODULATE modes.
     *
     * Before this, the backend received {@code blend = (blending == REPLACE)
     * ? 0 : 1}, so ALPHA, ALPHA_ADD, MODULATE and MODULATE_X2 all produced
     * the same SRC_ALPHA/ONE_MINUS_SRC_ALPHA result: a game using MODULATE
     * for shadows got a translucent square instead of a multiply, and a
     * depthWrite=false decal wrote depth and occluded what it covered.
     */
    public static native void setState(boolean cull, int blendMode,
                                       boolean depthTest, boolean depthWrite,
                                       boolean colorWrite, float alphaThreshold,
                                       int blendColor);

    /**
     * v04.41: M3G Fog of the appearance being drawn. Integer-encoded on
     * purpose: everything except float/double RETURNS already uses the integer
     * register convention (see vita_kni_fp_overlay), so passing distances as
     * 1/256 units keeps the argument ABI out of the picture.
     *
     * @param mode        0 = no fog, Fog.EXPONENTIAL (80), Fog.LINEAR (81)
     * @param color       0xRRGGBB
     * @param near256     near distance * 256
     * @param far256      far distance * 256
     * @param density1000 density * 1000
     */
    public static native void setFog(int mode, int color, int near256,
                                     int far256, int density1000);

    /**
     * One triangle list. uv may be null, indices may be null (sequential).
     * rgba is 4 bytes per vertex, texHandle 0 = untextured.
     */
    public static native void draw(float[] pos3, float[] uv2, byte[] rgba,
                            int[] indices, int indexCount, int texHandle);

    /** Read the frame back into argb (0xAARRGGBB). */
    public static native void present(int[] argb, int width, int height);

    /**
     * v03.84: CPU skinning. Deforms pos (n vertices, xyz) in place. mats holds 4x4 column-major bone matrices;
     * ent holds 4 ints per entry (matrix index or -1, first vertex, vertex count, weight). Returns false when the
     * native side refused the arguments, in which case pos is untouched.
     */
    public static native boolean skin(float[] pos, int n, float[] mats, int[] ent, int nent);

    /**
     * v04.20: per-vertex lighting for ambient and directional lights (see m3g_shade.h for the prm layout). Fills out
     * (n*4 bytes, RGBA). The normals / colours are a VertexArray's own storage: nI/cI (int[]) or nF/cF (float[]),
     * comps 0 = absent. Returns false when the native side refused the arguments, in which case out is untouched.
     */
    public static native boolean shade(byte[] out, int n, float[] prm, int[] nI, float[] nF, int nComps, int nSize,
                                       int[] cI, float[] cF, int cComps, int cSize, int nl);

    /**
     * v04.24: out[v*stride + c] = raw(v*comps + c) * sb[0] + sb[1 + c], stride 3 (positions) or 2 (texcoords); raw is
     * the float[] fData when not null, else iData converted to float. Returns false when refused, out untouched.
     */
    public static native boolean decode(float[] out, int n, int stride, int[] iData, float[] fData, int comps, float[] sb);

    /**
     * v04.24: skeleton matrices. local/bindInv/bm hold nk 4x4 column-major matrices; world[k] = world[parent[k]] * local[k]
     * (parent[k] below 0: local[k]); bm[k] = world[k] * bindInv[k] and state[k] (1 usable, 2 non-finite) for every k with used[k] != 0.
     */
    public static native boolean bones(float[] local, int[] parent, float[] bindInv, byte[] used, int nk, float[] bm,
                                       byte[] state);
}
