package javax.microedition.m3g;

import com.sun.m3g.Matrix4;
import com.sun.m3g.NativeBridge;

/**
 * M3G scene traversal and drawing.
 *
 * Lives in the API package on purpose: it reads the package-private state
 * of the scene classes, so the alternative would be widening their public
 * surface beyond JSR 184. Nothing here is public.
 *
 * Pipeline per frame:
 *   clear -> camera view -> projection -> walk the graph
 *   -> per mesh: MVP + per-vertex lighting/colour + texture -> backend
 *
 * Deliberately not implemented: skinning, fog application, multi-texture,
 * animation sampling, perspective-corrected sprites, POINT/LINE fill modes.
 */
final class Renderer {

    /** Lights registered through Graphics3D.addLight (world-space). */
    private static Light[] xLights = new Light[0];
    private static float[][] xLightMats = new float[0][];

    /** World time in milliseconds, advanced once per released frame. */
    static int worldTime = 0;
    private static long lastFrameUs = 0;

    /* v03.76: per-phase wall-clock totals (ms) over PT_WIN released frames. log157 showed ~1-2 frames/s after the
     * double-constant fix but nothing says where the time goes; System.currentTimeMillis is the only clock here, so
     * only totals over a window are meaningful. Index: 0 bind->release, 1 gap release->next bind (game logic),
     * 2 animateSelf, 3 vertex decode, 4 applySkin, 5 shade, 6 NativeBridge.draw, 7 present, 8 drawRGB.
     * v03.86 splits 4: 9 computeWorld, 10 bone matrices (mul + finite check), 11 pack + native/Java kernel. */
    static final int PT_WIN = 20;
    /* v04.24: false = decodeAttr() always runs the Java loop (defaults.cfg m3gnative=0, or after a refusal) */
    static boolean useNativeDecode = true;
    static final float[] decSb = new float[4];

    /**
     * out[v*stride + c] = raw(v, c) * scale + bias[c] for c below stride (3: positions, 2: texture coordinates), where
     * raw is VertexArray.componentRaw. Native when the kernel accepts it, the plain Java loop otherwise.
     */
    static void decodeAttr(float[] out, int n, int stride, VertexArray va, float scale, float[] bias) {
        if (useNativeDecode && va.numComponents >= stride) {
            decSb[0] = scale;
            decSb[1] = bias[0];
            decSb[2] = bias[1];
            decSb[3] = bias[2];
            int[] di = (va.floatData != null) ? null : va.data;
            if (NativeBridge.decode(out, n, stride, di, va.floatData, va.numComponents, decSb)) {
                return;
            }
            useNativeDecode = false;
            com.sun.m3g.M3gTrace.log("native decode refused; using the Java loop");
        }
        for (int i = 0; i < n; i++) {
            for (int c = 0; c < stride; c++) {
                out[i * stride + c] = va.componentRaw(i, c) * scale + bias[c];
            }
        }
    }

    /* v04.20: false = shade() always runs the Java loop (defaults.cfg m3gnative=0 or m3gshade=0, or after a refusal) */
    static boolean useNativeShade = true;
    static int shadeNativeN = 0;        /* shade() calls answered by the native kernel / by the Java loop */
    static int shadeJavaN = 0;
    static final long[] ptMs = new long[24];
    static final int[] ptN = new int[24];
    static int ptFrames = 0;
    static long ptBindAt = 0;
    static long ptReleaseAt = 0;

    static void ptAdd(int k, long t0) {
        ptMs[k] += System.currentTimeMillis() - t0;
        ptN[k]++;
    }

    static void ptReport() {
        ptFrames++;
        if (ptFrames % PT_WIN != 0) {
            return;
        }
        com.sun.m3g.M3gTrace.log("ptime ms=" + System.currentTimeMillis() + " frames=" + ptFrames
            + " per-frame ms: total=" + (ptMs[0] / PT_WIN) + " gap=" + (ptMs[1] / PT_WIN)
            + " anim=" + (ptMs[2] / PT_WIN) + "(x" + (ptN[2] / PT_WIN) + ") decode=" + (ptMs[3] / PT_WIN)
            + " skin=" + (ptMs[4] / PT_WIN) + "(x" + (ptN[4] / PT_WIN) + ") shade=" + (ptMs[5] / PT_WIN)
            + " draw=" + (ptMs[6] / PT_WIN) + "(x" + (ptN[6] / PT_WIN) + ") present=" + (ptMs[7] / PT_WIN)
            + " blit=" + (ptMs[8] / PT_WIN)
            + " | skin split: world=" + (ptMs[9] / PT_WIN) + " mats=" + (ptMs[10] / PT_WIN)
            + " kernel=" + (ptMs[11] / PT_WIN)
            + " | uv=" + (ptMs[12] / PT_WIN) + " mvp+state=" + (ptMs[13] / PT_WIN)
            + " drawMesh=" + (ptMs[14] / PT_WIN) + "(x" + (ptN[14] / PT_WIN) + ") walk=" + (ptMs[15] / PT_WIN)
            + " | api: render=" + (ptMs[16] / PT_WIN) + " clear=" + (ptMs[17] / PT_WIN) + " lights=" + (ptMs[18] / PT_WIN)
            + " release-other=" + (ptMs[20] / PT_WIN)
            + " game=" + ((ptMs[0] - ptMs[16] - ptMs[17] - ptMs[18] - ptMs[7] - ptMs[8] - ptMs[20]) / PT_WIN)
            + " | mesh: app+tex=" + (ptMs[21] / PT_WIN) + " mvp+idx=" + (ptMs[22] / PT_WIN)
            + " | shade calls native=" + shadeNativeN + " java=" + shadeJavaN);
        for (int i = 0; i < 24; i++) {
            ptMs[i] = 0;
            ptN[i] = 0;
        }
    }

    /* v03.22/03.27: bounded render statistics. "blank frame, no exception" was
     * impossible to diagnose from a device log because nothing recorded whether
     * the renderer ran at all, how many meshes it visited, or where the camera
     * ended up. These counters are reported by Graphics3D.releaseTarget().
     *
     * v03.27: the FIRST report now fires on the first releaseTarget regardless of
     * frame count. log132 showed "backend init rc=2" (software backend up) but no
     * "render frames=" line at all, and the old trigger needed 30 frames - so
     * "render() was never called" and "fewer than 30 frames" were
     * indistinguishable. Every 30th frame after that is still reported. */
    static int dbgFrames = 0;          /* frame() entered (camera accepted) */
    static int dbgRenderCalls = 0;      /* render(Node/World) entered at all */
    static int dbgNoCam = 0;            /* render() with no camera set       */
    static int dbgDraws = 0;            /* meshes actually submitted         */
    static int dbgVerts = 0;
    static int dbgNoView = 0;           /* camera matrix not invertible      */
    /* v04.49: last view matrix that WAS invertible, so one degenerate frame
     * (a title feeding a zero-length camera basis) does not blank the screen.
     * See the comment at the invert() call site. */
    private static float[] lastGoodView = null;
    static int dbgNoBuf = 0;            /* mesh without VertexBuffer         */
    static int dbgNoIdx = 0;            /* mesh without index buffer         */
    /* v03.28: renders that returned before frame() without being counted.
     * log133 (生化危机) printed "calls=1 frames=0" with every reject counter at
     * 0, so the early exit was one of these two silent paths. */
    static int dbgNoCamWorld = 0;       /* renderWorld(): no camera anywhere */
    static int dbgNullNode = 0;         /* renderNode(null, ...) -> NPE      */
    static int dbgReleases = 0;         /* releaseTarget() calls             */    static int dbgNodes = 0;            /* scene-graph nodes visited           */
    static int dbgMeshes = 0;           /* Mesh nodes seen by walk()           */
    static int dbgSprites = 0;          /* Sprite3D nodes seen by walk()       */
    private static int dbgNdcProbe = 0; /* clip-space reports emitted          */
    private static int dbgClearProbe = 0; /* clear-flag reports emitted        */
    private static int dbgCamDump = 0;  /* raw camera dumps emitted (v03.38)   */
    private static int dbgRootDump = 0; /* root transform dumps (v03.40)       */
    private static int dbgVertDump = 0; /* raw vertex dumps (v03.42)           */
    private static int dbgGameCam = 0;  /* gameplay camera samples (v03.50)    */
    private static int camMode = -1;    /* defaults.cfg m3gcam=, -1 = not read yet (v03.51) */
    private static int dbgSkinDump = 0;  /* SkinnedMesh full-matrix samples (v03.56) */
    private static int dbgGameMesh = 0; /* gameplay mesh samples (v03.50)      */

    /* v03.38: measured winding correction.
     * JSR-184 defines polygon winding in a screen space whose y axis points
     * DOWN (the MIDP painter space: Graphics also has y down), while a GL
     * window space has y pointing UP. The same vertex order therefore has the
     * opposite orientation, so GL_CULL_FACE + glFrontFace(GL_CCW) throws away
     * what M3G calls a front face - which produces exactly the symptom we are
     * stuck with: hundreds of draws, zero exceptions, not one pixel.
     * Rather than trusting my reading of the spec, measure the sign of the
     * projected triangle area and let that decide. -1 = not known yet. */
    private static int windingFix = -1;
    private static int dbgPosArea = 0, dbgNegArea = 0, dbgWindingSamples = 0;

    /** Short fixed-point float for the clip-space probe (no allocation storm). */
    private static String hx(float v) {
        return Integer.toHexString(Float.floatToIntBits(v));
    }

    private static String fmt(float v) {        if (v <= -1e29f) {
            return "?";
        }
        if (v >= 1e29f) {
            return "?";
        }
        int i = (int)(v * 100.0f);
        if (i > 1000000) {
            i = 1000000;
        }
        if (i < -1000000) {
            i = -1000000;
        }
        String s = Integer.toString(i);
        return s;
    }    private static int dbgLastFrames = -1;
    private static int dbgLastDraws = 0;
    private static int dbgLastReleases = 0;

    /** NDC x of vertex v (0 when behind the eye, so it never biases the sign). */
    private static float ndcX(float[] m, float[] pos, int v) {
        float x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
        float w = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (w <= 1e-5f) {
            return 0.0f;
        }
        return (m[0] * x + m[4] * y + m[8] * z + m[12]) / w;
    }

    /** NDC y of vertex v (0 when behind the eye). */
    private static float ndcY(float[] m, float[] pos, int v) {
        float x = pos[v * 3], y = pos[v * 3 + 1], z = pos[v * 3 + 2];
        float w = m[3] * x + m[7] * y + m[11] * z + m[15];
        if (w <= 1e-5f) {
            return 0.0f;
        }
        return (m[1] * x + m[5] * y + m[9] * z + m[13]) / w;
    }

    static void reportFrameStats(Graphics3D g) {
        /* v03.28: trigger on releaseTarget count, NOT on frames. The old
         * "frames advanced by 30" trigger can never fire when frames stays 0,
         * which is exactly the case that needs reporting (log133 printed once
         * and then went silent for thousands of frames). */
        dbgReleases++;
        /* Report the first few releases, then settle to every 120. The early
         * ones matter because the interesting frame is "the first frame of the
         * game view", which may be long before release 120. */
        if (dbgReleases > 3 && (dbgReleases - dbgLastReleases) < 60) {
            return;
        }
        dbgLastReleases = dbgReleases;
        com.sun.m3g.M3gTrace.log("render releases=" + dbgReleases
            + " noCamWorld=" + dbgNoCamWorld + " nullNode=" + dbgNullNode
            + " calls=" + dbgRenderCalls
            + " frames=" + dbgFrames
            + " (+draws=" + (dbgDraws - dbgLastDraws) + ")"
            + " verts=" + dbgVerts
            + " nodes=" + dbgNodes + " meshes=" + dbgMeshes
            + " sprites=" + dbgSprites
            + " nocam=" + dbgNoCam
            + " noview=" + dbgNoView + " nobuf=" + dbgNoBuf
            + " noidx=" + dbgNoIdx
            + " vp=" + g.vpOffsetX + "," + g.vpOffsetY + " "
            + g.vpDrawW + "x" + g.vpDrawH
            + " target=" + g.vpW + "x" + g.vpH);
        dbgLastFrames = dbgFrames;
        dbgLastDraws = dbgDraws;
    }

    static void advanceWorldTime() {
        long now = System.currentTimeMillis();
        if (lastFrameUs != 0) {
            long dt = now - lastFrameUs;
            if (dt > 0 && dt < 500) {
                worldTime += (int)dt;
            }
        }
        lastFrameUs = now;
    }

    private Renderer() {
    }


    static int addLight(Light light, Transform transform) {
        if (light == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        int n = xLights.length;
        Light[] nl = new Light[n + 1];
        float[][] nm = new float[n + 1][];
        System.arraycopy(xLights, 0, nl, 0, n);
        System.arraycopy(xLightMats, 0, nm, 0, n);
        nl[n] = light;
        nm[n] = transform != null ? copyOf(transform.m) : worldMatrix(light);
        xLights = nl;
        xLightMats = nm;
        return n;
    }

    static void resetLights() {
        xLights = new Light[0];
        xLightMats = new float[0][];
    }

    private static float[] copyOf(float[] src) {
        float[] r = new float[16];
        System.arraycopy(src, 0, r, 0, 16);
        return r;
    }

    static void setLight(int index, Light light, Transform transform) {
        if (index < 0 || index >= xLights.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        xLights[index] = light;
        xLightMats[index] = transform != null ? copyOf(transform.m)
                                              : worldMatrix(light);
    }


    /*
     * v04.43: MorphingMesh weights. Until now the base vertex buffer was drawn
     * and the targets were ignored (MorphingMesh.java said so in its header).
     * The M3G rule is
     *     v = base + sum_i w_i * (target_i - base)
     * with the weights stored on the MorphingMesh. Target buffers may carry
     * their own scale/bias and may have fewer vertices, so the blend covers
     * min(base, target) vertices - the rest keep the base position.
     */
    private static void applyMorph(float[] pos, int n, MorphingMesh mm) {
        float[] w = mm.weights;
        if (w == null || w.length == 0) {
            return;
        }
        int nTargets = mm.getMorphTargetCount();
        if (nTargets > w.length) {
            nTargets = w.length;
        }
        if (nTargets <= 0) {
            return;
        }
        /* Snapshot the base: every target is measured against the ORIGINAL
         * positions, so blending in place against a mutating buffer would
         * compound the earlier targets. */
        float[] base = new float[n * 3];
        System.arraycopy(pos, 0, base, 0, n * 3);
        float[] tmp = new float[n * 3];
        for (int t = 0; t < nTargets; t++) {
            float wt = w[t];
            if (wt == 0.0f) {
                continue;
            }
            VertexBuffer tv = mm.getMorphTarget(t);
            if (tv == null || tv.positions == null) {
                continue;
            }
            int m = (tv.numVertices < n) ? tv.numVertices : n;
            decodeAttr(tmp, m, 3, tv.positions, tv.positionScale, tv.positionBias);
            int lim = m * 3;
            for (int i = 0; i < lim; i++) {
                pos[i] += wt * (tmp[i] - base[i]);
            }
        }
    }

    /* v04.44: layer of submesh `s` (M3G default 0; -1 = the specification's
     * "unlayered", i.e. do not render this submesh at all). */
    private static int layerOf(Mesh mesh, int s) {
        if (mesh.appearances == null || s < 0 || s >= mesh.appearances.length) {
            return 0;
        }
        Appearance ap = mesh.appearances[s];
        return (ap != null) ? ap.getLayer() : 0;
    }

    static void clear(Graphics3D g, Background bg) {
        int color = (bg != null) ? bg.getColor() : 0xFF000000;
        boolean depth = (bg == null) || bg.isDepthClearEnabled();
        boolean colorClear = (bg == null) || bg.isColorClearEnabled();
        /* v03.24: force the clear colour opaque.
         *
         * 生化危机's setup does `background.setColor(0)` - ARGB 0x00000000,
         * i.e. alpha 0 - meaning "clear to black". We passed that alpha
         * straight through, so the whole viewport ended up fully transparent
         * in the frame buffer, and Graphics3D.releaseTarget() blits it with
         * Graphics.drawRGB(..., true), which SKIPS transparent pixels. The
         * result was a 3D area that looked untouched: the 2D HUD (drawn
         * directly on the Graphics afterwards) stayed visible while the scene
         * never appeared, with no exception anywhere in the log.
         * The drawing surface has no alpha channel of its own, so a
         * transparent background cannot mean "let the 2D content show
         * through"; treating the top byte as opaque matches what these titles
         * expect and what handset implementations do. */
        color |= 0xFF000000;
        /* v03.36: log the clear flags for the first few clears. If the title
         * turns the depth clear off, the FBO's depth renderbuffer keeps whatever
         * it had (0 on a fresh allocation), and with GL_DEPTH_TEST + GL_LEQUAL
         * every fragment then fails and the readback contains only the clear
         * colour - one of the two ways to get "297 draws, zero pixels". */
        if (dbgClearProbe < 6) {
            dbgClearProbe++;
            com.sun.m3g.M3gTrace.log("clear color=0x"
                + Integer.toHexString(color) + " depthClear=" + depth
                + " colorClear=" + colorClear + " vp=" + g.vpDrawW + "x"
                + g.vpDrawH);
        }
        NativeBridge.clear(color, depth, colorClear);
        if (colorClear && bg != null && bg.getImage() != null) {
            drawBackgroundImage(g, bg.getImage());
        }
    }

    /* v03.53: Background.setImage was stored but never drawn. BioCtrl.setBGTexture hands every room's pre-rendered bg*.png
     * (120x90, exactly half of the 240x180 gameplay viewport) to Background.setImage and clears with it each frame, so the
     * whole backdrop was missing. Draw it as a full-viewport quad just behind everything else, stretched over the viewport
     * (no crop/repeat: the title never calls setCrop/setImageMode). The crop rectangle is assumed to be the whole image. */
    private static int dbgBgImage = 0;

    private static void drawBackgroundImage(Graphics3D g, Image2D img) {
        if (g.vpDrawW <= 0 || g.vpDrawH <= 0 || g.vpW <= 0 || g.vpH <= 0) {
            return;
        }
        int tex = img.getTextureHandle();
        if (tex == 0) {
            return;
        }
        float sx = (float)g.vpDrawW / (float)g.vpW;
        float sy = (float)g.vpDrawH / (float)g.vpH;
        float cx = (float)(2 * g.vpOffsetX + g.vpDrawW) / (float)g.vpW - 1.0f;
        float cy = 1.0f - (float)(2 * g.vpOffsetY + g.vpDrawH) / (float)g.vpH;
        float l = -sx + cx, r = sx + cx, b = -sy + cy, t = sy + cy;
        float z = 0.9999f;
        float[] pos = {
            l, b, z,  r, b, z,  l, t, z,
            r, b, z,  r, t, z,  l, t, z
        };
        /* M3G texture origin is top-left: v = 0 at the top edge. */
        float[] uv = {
            0, 1,  1, 1,  0, 0,
            1, 1,  1, 0,  0, 0
        };
        byte[] rgba = new byte[24];
        for (int i = 0; i < 24; i++) {
            rgba[i] = (byte)255;
        }
        float[] id = new float[16];
        Matrix4.identity(id);
        NativeBridge.setMvp(id);
        /* v04.02: the background quad is opaque and must still write depth -
         * it is drawn behind everything else and the meshes rely on the depth
         * buffer to reject it where they cover it. REPLACE + depth on is
         * exactly what the removed `setState(false, false)` meant. */
        NativeBridge.setState(false, CompositingMode.REPLACE, true, true, true,
                              0.0f, 0);
        NativeBridge.draw(pos, uv, rgba, null, 6, tex);
        if (dbgBgImage < 3) {
            dbgBgImage++;
            com.sun.m3g.M3gTrace.log("bgimg " + img.getWidth() + "x" + img.getHeight() + " tex=" + tex
                + " vp=" + g.vpOffsetX + "," + g.vpOffsetY + " " + g.vpDrawW + "x" + g.vpDrawH);
        }
    }

    // ---------------------------------------------------------------- world

    static void renderWorld(Graphics3D g, World world) {
        dbgRenderCalls++;
        if (world == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        /* JSR-184: the camera most recently passed to Graphics3D.setCamera is
         * the one that renders; the World's active camera is only the default
         * when setCamera has never been called. v03.21 and earlier ignored
         * g.camera here entirely, so a title that only calls setCamera (the
         * CAPCOM title does, via BioCtrl.D_Game_JSR184) rendered nothing.
         * The transform given to setCamera replaces the camera's own node
         * transform, so it must reach frame() as well. */
        Camera cam = g.cameraInternal();
        Transform camXform = null;
        if (cam != null && g.cameraFromTransformInternal()) {
            camXform = g.cameraTransformInternal();
        }
        if (cam == null) {
            cam = world.getActiveCamera();
        }
        if (cam == null) {
            dbgNoCamWorld++;
            return; /* the spec draws nothing without a camera */
        }
        clear(g, world.getBackground());
        frame(g, world, cam, null, camXform, false);
    }

    /* v03.58: per-entity state of the root matrix handed to render(node, t). log152: the game's facing matrix is sometimes
     * diag(0,1,0) (two of three columns vanish), which squashes the model and zeroes the walking direction (m02,m12,m22).
     * Report only CHANGES, with the wall clock the [INPUT] lines in input_debug.log now carry, so key presses can be lined up. */
    private static final int RS_MAX = 24;
    private static final int[] rsNode = new int[RS_MAX];
    private static final int[] rsPx = new int[RS_MAX];
    private static final int[] rsPz = new int[RS_MAX];
    private static final byte[] rsState = new byte[RS_MAX];
    private static final long[] rsAt = new long[RS_MAX];
    private static int rsUsed = 0;
    private static int rsNext = 0;
    private static int rsLines = 0;

    private static void trackRoot(Node node, Transform t) {
        if (t == null || rsLines >= 400 || !(node instanceof Mesh)) {
            return;
        }
        float[] m = t.m;
        float n0 = (float)Math.sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
        float n1 = (float)Math.sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
        float n2 = (float)Math.sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
        float big = n0 > n1 ? n0 : n1;
        if (n2 > big) {
            big = n2;
        }
        int cols = 0;
        if (big > 1e-9f) {
            if (n0 > 0.25f * big) cols++;
            if (n1 > 0.25f * big) cols++;
            if (n2 > 0.25f * big) cols++;
        }
        int id = node.hashCode();
        int px = (int)(m[12] * 1000.0f);
        int pz = (int)(m[14] * 1000.0f);
        /* v03.76: one slot per mesh (was per mesh+position, which filled all 24 slots in ~80 s of play in log157). A
         * line is written when the column count changes or the mesh moved >= 100 (0.1 unit) since the last line, and
         * at most every 400 ms per mesh. */
        long now = System.currentTimeMillis();
        int slot = -1;
        for (int i = 0; i < rsUsed; i++) {
            if (rsNode[i] == id) {
                slot = i;
                break;
            }
        }
        boolean fresh = false;
        if (slot < 0) {
            slot = rsNext % RS_MAX;
            rsNext++;
            if (rsUsed < RS_MAX) {
                rsUsed++;
            }
            rsNode[slot] = id;
            fresh = true;
        } else {
            int dx = px - rsPx[slot];
            int dz = pz - rsPz[slot];
            boolean moved = dx >= 100 || dx <= -100 || dz >= 100 || dz <= -100;
            if (rsState[slot] == cols && (!moved || now - rsAt[slot] < 400)) {
                return;
            }
        }
        int prev = fresh ? -1 : rsState[slot];
        rsPx[slot] = px;
        rsPz[slot] = pz;
        rsAt[slot] = now;
        rsState[slot] = (byte)cols;
        rsLines++;
        com.sun.m3g.M3gTrace.log("rootstate#" + rsLines + " ms=" + System.currentTimeMillis() + " mesh=" + Integer.toHexString(id)
            + " n=" + ((Mesh)node).vertexBuffer.numVertices + " pos*1000=" + px + "," + (int)(m[13] * 1000.0f) + "," + pz
            + " colnorm*1e4=" + (int)(n0 * 10000.0f) + "," + (int)(n1 * 10000.0f) + "," + (int)(n2 * 10000.0f)
            + " cols " + prev + "->" + cols + (fresh ? " (new position/entity)" : ""));
    }

    static void renderNode(Graphics3D g, Node node, Transform transform) {
        dbgRenderCalls++;
        if (node == null) {
            dbgNullNode++;
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        trackRoot(node, transform);
        Camera cam = g.getCamera((Transform)null);
        if (cam == null) {
            dbgNoCam++;
            return;
        }
        /* Same as renderWorld: honour the transform that came with
         * setCamera(camera, transform). Without it the view matrix was built
         * from the camera node's own transform (usually identity), which put
         * the eye at the world origin - the scene was outside the frustum and
         * the framebuffer stayed empty with no error at all. */
        Transform camXform = g.cameraFromTransformInternal()
            ? g.cameraTransformInternal() : null;
        frame(g, node, cam, transform, camXform, true);
    }

    private static void frame(Graphics3D g, Node root, Camera cam,
                              Transform rootTransform, Transform camXform, boolean ignoreOwn) {
        dbgFrames++;
        /* v03.38: BACK to inverting -- v03.36 was my own regression.
         *
         * v03.36 removed the inversion because I read setCamera(camera,
         * transform) as "the transform is already the view matrix". log138
         * disproved it outright: "ndc n=472 behind=472 inside=0", i.e. EVERY
         * vertex had clip w <= 0 (all behind the eye).
         *
         * The bytecode settles the direction. BioCtrl has three setCamera
         * sites; two of them build the transform out of nothing but a
         * translation (javap -c BioCtrl, non-obfuscated):
         *
         *   t.postTranslate(-9.0f, 90.0f, 180.0f);
         *   camera.setPerspective(70.0f, 1.7142857f, 1.0f, 10000.0f);
         *   g3d.setCamera(camera, t);
         *
         *   t.postTranslate(-8.0f, 16.0f, 32.0f);   // or (-15, 18, 30)
         *   camera.setPerspective(70.0f, 1.2f, 1.0f, 10000.0f);
         *   g3d.setCamera(camera, t);
         *
         * There is no rotation and no scale: those numbers are the CAMERA
         * POSITION in world space - the map view puts the eye 32 units up in
         * z, the in-game view 180. setCamera(camera, transform) supplies the
         * transform applied TO the camera node (camera space -> world space),
         * exactly like Node.setTransform; null means "use the camera's own
         * world transform". Either way the VIEW matrix is its inverse. Handed
         * over un-inverted, a pure translate(+180 z) leaves every vertex at
         * z_view >= 180 -- behind the eye. That was log138.
         *
         * Note also far=10000 and near=1 from the bytecode, not the 0.1..75 I
         * had assumed in v03.36. */
        float[] camWorld = (camXform != null) ? copyOf(camXform.m)
                                             : worldMatrix(cam);
        float[] view = new float[16];
        /* v03.51: the camera convention is switchable from defaults.cfg (m3gcam=inv|direct|invt) because log146 could
         * not tell them apart: with "inv" all 7 sampled gameplay meshes sit behind the eye (eye z > 0). Default stays
         * "inv", i.e. exactly the previous behaviour. */
        if (camMode < 0) {
            int raw = 0;
            try {
                raw = NativeBridge.camMode();
            } catch (Throwable t) {
                raw = 0;
            }
            camMode = raw & 0xFF;
            SkinnedMesh.useNative = ((raw >> 16) & 1) == 0;
            SkinnedMesh.useNativeBones = SkinnedMesh.useNative;
            useNativeShade = ((raw >> 16) & 3) == 0;
            useNativeDecode = ((raw >> 16) & 1) == 0;
            com.sun.m3g.M3gTrace.log("camMode=" + (camMode == 1 ? "direct" : camMode == 2 ? "invt" : "inv")
                + " nativeSkin=" + SkinnedMesh.useNative);
        }
        float[] camSrc = camWorld;
        if (camMode == 2) {
            camSrc = new float[16];
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    camSrc[c * 4 + r] = camWorld[r * 4 + c];
                }
            }
        }
        if (camMode == 1) {
            System.arraycopy(camWorld, 0, view, 0, 16);
        } else if (!Matrix4.invert(view, camSrc)) {
            /* v04.49: an unusable camera must not cost us the whole frame.
             *
             * log189: the title's camera basis is normalised from two 3D points
             * (ai.f() -> ab.a(Transform,int x6)); when the points coincide the
             * scale is +Inf and camWorld is all NaN, so invert fails. This used
             * to `return` here, which means the frame produced NO draw calls at
             * all - a totally black screen, which is what the user reported.
             *
             * Reusing the last good view matrix keeps the previous frame's
             * geometry on screen for the one frame the title's data is
             * degenerate. That is what the title would have shown on a device
             * whose m3g core left the matrix untouched, and it degrades to
             * "camera lags a frame" instead of "screen goes black". On the very
             * first frame there is no previous matrix, so identity is used -
             * still draws something rather than nothing. */
            dbgNoView++;
            if (lastGoodView != null) {
                System.arraycopy(lastGoodView, 0, view, 0, 16);
            } else {
                Matrix4.identity(view);
            }
            if (dbgNoView <= 3) {
                com.sun.m3g.M3gTrace.log("gview: camera matrix not invertible, "
                    + "reusing " + (lastGoodView != null ? "previous view" : "identity")
                    + " (count=" + dbgNoView + ")");
            }
        } else {
            if (lastGoodView == null) {
                lastGoodView = new float[16];
            }
            System.arraycopy(view, 0, lastGoodView, 0, 16);
        }
        /* v03.38: one-shot raw camera numbers. If a later log is still black,
         * these are the ground truth rather than my reading of the spec. */
        if (dbgCamDump < 2) {
            dbgCamDump++;
            com.sun.m3g.M3gTrace.log("cam " + (camXform == null ? "node" : "xform")
                + " eye=" + camWorld[12] + "," + camWorld[13] + ","
                + camWorld[14]
                + " m00=" + camWorld[0] + " m11=" + camWorld[5]
                + " m22=" + camWorld[10]);
        }
        /* v03.50: log145 - every triangle of the in-game frames (viewport 0,40 240x180) dies in clip_w, i.e. w <= 0 for all
         * three vertices, while the 12 mission-map draws rasterise fine. The two probes above stopped after 2-3 reports, all of
         * them map frames, so nothing is known about the gameplay camera. Sample it every 30th frame, bounded to 12 reports,
         * and only once the viewport is the gameplay one (not the 0,105 map rect). Bit patterns, not Float.toString. */
        if (dbgGameCam < 12 && g.vpDrawH != 140 && (dbgFrames % 30) == 1) {
            dbgGameCam++;
            float[] gm = new float[16];
            try {
                gm = camWorld;
                com.sun.m3g.M3gTrace.log("gcam#" + dbgGameCam + " f=" + dbgFrames + " vp=" + g.vpDrawW + "x" + g.vpDrawH
                    + " " + (camXform == null ? "node" : "xform")
                    + " camW r0=" + hx(gm[0]) + "," + hx(gm[4]) + "," + hx(gm[8])
                    + " r1=" + hx(gm[1]) + "," + hx(gm[5]) + "," + hx(gm[9])
                    + " r2=" + hx(gm[2]) + "," + hx(gm[6]) + "," + hx(gm[10])
                    + " t=" + hx(gm[12]) + "," + hx(gm[13]) + "," + hx(gm[14])
                    + " w=" + hx(gm[3]) + "," + hx(gm[7]) + "," + hx(gm[11]) + "," + hx(gm[15]));
                com.sun.m3g.M3gTrace.log("gcam#" + dbgGameCam + " view t=" + hx(view[12]) + "," + hx(view[13]) + "," + hx(view[14])
                    + " w=" + hx(view[3]) + "," + hx(view[7]) + "," + hx(view[11]) + "," + hx(view[15])
                    + " ; root=" + (rootTransform == null ? "null"
                        : hx(rootTransform.m[0]) + "," + hx(rootTransform.m[5]) + "," + hx(rootTransform.m[10])
                          + " t=" + hx(rootTransform.m[12]) + "," + hx(rootTransform.m[13]) + "," + hx(rootTransform.m[14])));
            } catch (Throwable t) {
                com.sun.m3g.M3gTrace.log("gcam FAILED " + t);
            }
        }
        /* v03.40: the root transform the title passes to render(node, t). Its
         * translation is what the post* ordering decides, so log it raw. */
        if (rootTransform != null && dbgRootDump < 3) {
            dbgRootDump++;
            com.sun.m3g.M3gTrace.log("root t=" + rootTransform.m[12] + ","
                + rootTransform.m[13] + "," + rootTransform.m[14]
                + " s=" + rootTransform.m[0] + "," + rootTransform.m[5] + ","
                + rootTransform.m[10]);
        }
        /* Aspect from the viewport, not the whole target: a title that
         * draws the 3D scene into a sub-rectangle expects the scene to fill
         * that rectangle, not to be letterboxed inside the full screen. */
        float[] proj = projection(cam, g.vpDrawW, g.vpDrawH);
        if (g.vpDrawW <= 0 || g.vpDrawH <= 0) {
            return;   /* empty viewport */
        }
        /* Map clip space into the viewport rectangle: without this the
         * scene would be drawn across the whole framebuffer and then
         * clipped away, leaving a black rectangle. */
        {
            float sx = (float)g.vpDrawW / (float)g.vpW;
            float sy = (float)g.vpDrawH / (float)g.vpH;
            float cx = (float)(2 * g.vpOffsetX + g.vpDrawW) / (float)g.vpW - 1.0f;
            float cy = 1.0f - (float)(2 * g.vpOffsetY + g.vpDrawH) / (float)g.vpH;
            float[] vp = {
                sx, 0, 0, 0,
                0, sy, 0, 0,
                0, 0, 1, 0,
                cx, cy, 0, 1
            };
            float[] tmp = new float[16];
            Matrix4.mul(tmp, vp, proj);
            System.arraycopy(tmp, 0, proj, 0, 16);
        }

        int nWorld = 0;
        if (root instanceof World) {
            World w = (World)root;
            nWorld = w.getLightCount();
        }
        int total = nWorld + xLights.length;
        Light[] lights = new Light[total];
        float[][] lmats = new float[total][];
        for (int i = 0; i < nWorld; i++) {
            lights[i] = ((World)root).getLight(i);
            lmats[i] = worldMatrix(lights[i]);
        }
        for (int i = 0; i < xLights.length; i++) {
            lights[nWorld + i] = xLights[i];
            lmats[nWorld + i] = xLightMats[i];
        }

        float[] base = new float[16];
        if (rootTransform != null) {
            System.arraycopy(rootTransform.m, 0, base, 0, 16);
        } else {
            Matrix4.identity(base);
        }
        long ptwk = System.currentTimeMillis();
        walk(g, root, base, view, proj, lights, lmats, ignoreOwn);
        ptAdd(15, ptwk);
    }

    private static void walk(Graphics3D g, Node node, float[] model,
                             float[] view, float[] proj, Light[] lights,
                             float[][] lmats, boolean ignoreOwn) {
        dbgNodes++;
        float[] local = new float[16];
        if (ignoreOwn) {
            /* JSR-184 render(Node, Transform): the node's own transformation is ignored, the given one replaces it. */
            Matrix4.identity(local);
        } else {
            node.getCompositeMatrix(local);
        }
        float[] m = new float[16];
        Matrix4.mul(m, model, local);

        if (node.rendering) {
            long pt = System.currentTimeMillis();
            node.animateSelf(worldTime);
            ptAdd(2, pt);
        }

        if (node instanceof Mesh) {
            dbgMeshes++;
            long ptdm = System.currentTimeMillis();
            drawMesh(g, (Mesh)node, m, view, proj, lights, lmats);
            ptAdd(14, ptdm);
        } else if (node instanceof Sprite3D) {
            dbgSprites++;
            drawSprite(g, (Sprite3D)node, m, view, proj);
        }
        if (node instanceof Group) {
            Group grp = (Group)node;
            for (int i = 0; i < grp.childCountInternal(); i++) {
                walk(g, grp.childAtInternal(i), m, view, proj, lights, lmats, false);
            }
        }
    }

    // ---------------------------------------------------------------- mesh

    private static void drawMesh(Graphics3D g, Mesh mesh, float[] model,
                                 float[] view, float[] proj, Light[] lights,
                                 float[][] lmats) {
        VertexBuffer vb = mesh.vertexBuffer;
        if (vb == null || vb.positions == null) {
            dbgNoBuf++;
            return;
        }
        /* v03.30: draw EVERY submesh. This used to look only at
         * mesh.indexBuffer / mesh.appearances[0], i.e. submesh 0; a mesh
         * built with the JSR-184 array constructor (Need for Speed) or a
         * multi-material .m3g model silently lost the other submeshes.
         * Per submesh: its own index buffer and Appearance (texture, cull,
         * blend, material). Per VertexBuffer: positions, which are computed
         * once. Shading depends on the submesh's Material, so it is cached
         * per distinct Material; UVs depend on whether the submesh has a
         * texture, so they are computed lazily and once. */
        IndexBuffer[] subs = mesh.indexBuffers;
        if (subs == null || subs.length == 0) {
            dbgNoIdx++;
            return;
        }
        int nSub = subs.length;

        /* v04.44: Appearance.layer. M3G renders the submeshes in ascending
         * layer order and SKIPS layer == -1 ("unlayered" in the specification =
         * do not draw). The sort is STABLE, so submeshes sharing a layer keep
         * their definition order - which is what models that rely on layering
         * (decals over a body, glass in front of a room) expect.
         * Insertion sort: nSub is single digits in every model seen so far. */
        int[] order = new int[nSub];
        int nDraw = 0;
        for (int s = 0; s < nSub; s++) {
            if (layerOf(mesh, s) >= 0) {
                order[nDraw++] = s;
            }
        }
        for (int i = 1; i < nDraw; i++) {
            int v = order[i];
            int lv = layerOf(mesh, v);
            int j = i - 1;
            while (j >= 0 && layerOf(mesh, order[j]) > lv) {
                order[j + 1] = order[j];
                j--;
            }
            order[j + 1] = v;
        }

        long ptv = System.currentTimeMillis();
        float[] mvp = new float[16];
        Matrix4.mul(mvp, view, model);
        Matrix4.mul(mvp, proj, mvp);
        ptAdd(22, ptv);

        int n = vb.numVertices;
        float[] pos = null;           /* built on the first drawable submesh */
        float[] uv = null;
        boolean uvDone = false;
        Material shadedFor = null;    /* Material the cached rgba belongs to */
        byte[] rgba = null;
        boolean shaded = false;
        boolean skinDumped = false;

        for (int oi = 0; oi < nDraw; oi++) {
            int s = order[oi];
            IndexBuffer ib = subs[s];
            if (ib == null) {
                dbgNoIdx++;
                continue;
            }
            int[] idx = ib.triangleList();
            if (idx == null || idx.length < 3) {
                dbgNoIdx++;
                continue;
            }
            long pta = System.currentTimeMillis();
            Appearance ap = (mesh.appearances != null && s < mesh.appearances.length)
                ? mesh.appearances[s] : null;
            int texHandle = 0;
            int texNearest = 0;
            int texWrap = 0;
            int cull = 0;
            /* v04.02: the whole CompositingMode, not just a blend on/off bit.
             * The defaults below are M3G's own defaults for a missing
             * CompositingMode (REPLACE, depth test+write on, colour write on,
             * no threshold), i.e. exactly what the backend did before. */
            int blend = CompositingMode.REPLACE;
            boolean depthTest = true;
            boolean depthWrite = true;
            boolean colorWrite = true;
            float alphaThreshold = 0.0f;
            /* v04.41: M3G Fog of this appearance (0 = no fog). Distances are
             * carried as *256 and density as *1000 so the KNI call stays
             * integer-only (see NativeBridge.setFog). */
            int fogMode = 0;
            int fogColor = 0;
            int fogNear256 = 0;
            int fogFar256 = 1 << 20;
            int fogDensity1000 = 0;
            int blendColor = 0;
            boolean windingCw = false;
            Material mat = null;
            if (ap != null) {
                PolygonMode pm = ap.getPolygonMode();
                if (pm != null) {
                    cull = (pm.getCulling() == PolygonMode.CULL_NONE) ? 0 : 1;
                    windingCw = (pm.getWinding() == PolygonMode.WINDING_CW);
                }
                CompositingMode cm = ap.getCompositingMode();
                if (cm != null) {
                    blend = cm.getBlending();
                    depthTest = cm.isDepthTestEnabled();
                    depthWrite = cm.isDepthWriteEnabled();
                    colorWrite = cm.isColorWriteEnabled();
                    alphaThreshold = cm.getAlphaThreshold();
                    blendColor = cm.getBlendColor();
                }
                Texture2D t = ap.getTexture(0);
                if (t != null && t.getImage() != null) {
                    texHandle = t.getImage().getTextureHandle();
                    texNearest = (t.imageFilter == Texture2D.FILTER_NEAREST) ? 1 : 0;
                    /* v04.35: bit 1/2 above the handle = WRAP_CLAMP for S / T. M3G defaults to
                     * REPEAT, so only CLAMP has to travel down; the CLAMP_TO_EDGE sampler is what
                     * stops an atlas from pulling in the opposite edge on UVs just past 1.0. */
                    texWrap = ((t.wrapS == Texture2D.WRAP_CLAMP) ? 1 : 0)
                            | ((t.wrapT == Texture2D.WRAP_CLAMP) ? 2 : 0);
                }
                mat = ap.getMaterial();
                /* v04.41: fog is per appearance, like the material. */
                Fog fog = ap.getFog();
                if (fog != null) {
                    fogMode = fog.getMode();
                    fogColor = fog.getColor() & 0xFFFFFF;
                    fogNear256 = (int) (fog.getNearDistance() * 256.0f);
                    fogFar256 = (int) (fog.getFarDistance() * 256.0f);
                    fogDensity1000 = (int) (fog.getDensity() * 1000.0f);
                } else {
                    fogMode = 0;
                }
            }
            ptAdd(21, pta);
            /* v03.38: apply the measured orientation correction on top of the
             * per-appearance WINDING_CW flip. Two flips cancel out. */
            boolean flipW = windingCw;
            if (cull != 0 && windingFix == 1) {
                flipW = !flipW;
            }
            if (flipW) {
                idx = flipWinding(idx);
            }

            if (pos == null) {
                long ptd = System.currentTimeMillis();
                pos = new float[n * 3];
                /* File-format position encode: value * scale + bias. */
                decodeAttr(pos, n, 3, vb.positions, vb.positionScale, vb.positionBias);
                ptAdd(3, ptd);
                if (mesh instanceof MorphingMesh) {
                    applyMorph(pos, n, (MorphingMesh) mesh);
                }
                if (mesh instanceof SkinnedMesh) {
                    long pts = System.currentTimeMillis();
                    ((SkinnedMesh)mesh).applySkin(pos, n);
                    ptAdd(4, pts);
                }
            }
            if (texHandle != 0 && vb.texCoords != null && !uvDone) {
                uvDone = true;
                uv = new float[n * 2];
                long ptu = System.currentTimeMillis();
                decodeAttr(uv, n, 2, vb.texCoords, vb.texScale, vb.texBias);
                ptAdd(12, ptu);
            }
            if (!shaded || mat != shadedFor) {
                long ptl = System.currentTimeMillis();
                rgba = shade(vb, model, lights, lmats, mat);
                ptAdd(5, ptl);
                shadedFor = mat;
                shaded = true;
            }

            long ptm = System.currentTimeMillis();
            NativeBridge.setMvp(mvp);
            NativeBridge.setState(cull != 0, blend, depthTest, depthWrite,
                                  colorWrite, alphaThreshold, blendColor);
            NativeBridge.setFog(fogMode, fogColor, fogNear256, fogFar256,
                                fogDensity1000);
            ptAdd(13, ptm);
            dbgDraws++;
            dbgVerts += idx.length;

            /* v03.50: gameplay mesh sample (see gcam in frame()): first vertex through model, view and projection, so
             * "where does w go negative" is answerable. Every 200th draw, bounded to 16 reports. */
            if (dbgGameMesh < 16 && g.vpDrawH != 140 && (dbgDraws % 200) == 1) {
                dbgGameMesh++;
                try {
                    float x0 = pos[0], y0 = pos[1], z0 = pos[2];
                    float wx = model[0] * x0 + model[4] * y0 + model[8] * z0 + model[12];
                    float wy = model[1] * x0 + model[5] * y0 + model[9] * z0 + model[13];
                    float wz = model[2] * x0 + model[6] * y0 + model[10] * z0 + model[14];
                    float vx = view[0] * wx + view[4] * wy + view[8] * wz + view[12];
                    float vy = view[1] * wx + view[5] * wy + view[9] * wz + view[13];
                    float vz = view[2] * wx + view[6] * wy + view[10] * wz + view[14];
                    float cw0 = mvp[3] * x0 + mvp[7] * y0 + mvp[11] * z0 + mvp[15];
                    com.sun.m3g.M3gTrace.log("gmesh#" + dbgGameMesh + " d=" + dbgDraws + " n=" + n + " idx=" + idx.length
                        + " tex=" + texHandle + " cull=" + cull + " blend=" + blend
                        + " v0=" + hx(x0) + "," + hx(y0) + "," + hx(z0)
                        + " world=" + hx(wx) + "," + hx(wy) + "," + hx(wz)
                        + " eye=" + hx(vx) + "," + hx(vy) + "," + hx(vz) + " w=" + hx(cw0));
                    com.sun.m3g.M3gTrace.log("gmesh#" + dbgGameMesh + " model m=" + hx(model[0]) + "," + hx(model[5]) + ","
                        + hx(model[10]) + " t=" + hx(model[12]) + "," + hx(model[13]) + "," + hx(model[14])
                        + " proj m0=" + hx(proj[0]) + " m5=" + hx(proj[5]) + " m10=" + hx(proj[10]) + " m11=" + hx(proj[11])
                        + " m14=" + hx(proj[14]) + " m15=" + hx(proj[15]));
                } catch (Throwable t) {
                    com.sun.m3g.M3gTrace.log("gmesh FAILED " + t);
                }
            }

            /* v03.56: log150/151 Jill draws rasterise 400+ triangles and write 0 pixels, while only the diagonal of her model
             * matrix is logged. Dump the full matrix (replayable on the host) and the screen box of her local AABB,
             * which does not depend on triangles. */
            if (!skinDumped && mesh instanceof SkinnedMesh && g.vpDrawH != 140 && dbgSkinDump < 6
                && (dbgFrames % 30) == 1) {
                skinDumped = true;
                dbgSkinDump++;
                try {
                    StringBuffer sb = new StringBuffer();
                    for (int k = 0; k < 16; k++) {
                        if (k > 0) sb.append(',');
                        sb.append(hx(model[k]));
                    }
                    float lx0 = 1e30f, ly0 = 1e30f, lz0 = 1e30f, lx1 = -1e30f, ly1 = -1e30f, lz1 = -1e30f;
                    for (int i = 0; i < n; i++) {
                        float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
                        if (x < lx0) lx0 = x;
                        if (x > lx1) lx1 = x;
                        if (y < ly0) ly0 = y;
                        if (y > ly1) ly1 = y;
                        if (z < lz0) lz0 = z;
                        if (z > lz1) lz1 = z;
                    }
                    float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
                    int behindC = 0;
                    for (int c = 0; c < 8; c++) {
                        float x = ((c & 1) != 0) ? lx1 : lx0;
                        float y = ((c & 2) != 0) ? ly1 : ly0;
                        float z = ((c & 4) != 0) ? lz1 : lz0;
                        float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
                        float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
                        float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
                        if (cw <= 1e-5f) {
                            behindC++;
                            continue;
                        }
                        float sxp = (cx / cw * 0.5f + 0.5f) * (float)g.vpW;
                        float syp = (0.5f - cy / cw * 0.5f) * (float)g.vpH;
                        if (sxp < bx0) bx0 = sxp;
                        if (sxp > bx1) bx1 = sxp;
                        if (syp < by0) by0 = syp;
                        if (syp > by1) by1 = syp;
                    }
                    com.sun.m3g.M3gTrace.log("skin#" + dbgSkinDump + " f=" + dbgFrames + " n=" + n + " model=" + sb);
                    com.sun.m3g.M3gTrace.log("skin#" + dbgSkinDump + " local x[" + hx(lx0) + "," + hx(lx1) + "] y[" + hx(ly0) + ","
                        + hx(ly1) + "] z[" + hx(lz0) + "," + hx(lz1) + "] aabb px[" + (int)bx0 + "," + (int)by0 + ".." + (int)bx1
                        + "," + (int)by1 + "] behind=" + behindC);
                } catch (Throwable t) {
                    com.sun.m3g.M3gTrace.log("skin FAILED " + t);
                }
            }

            /* v03.36: WHERE the geometry ends up in clip space.
             * log137 settled that the draws produce no pixels at all: the
             * readback probe reported "first=0x00000000 centre=0xff000000
             * alpha!=0 == distinct", i.e. the framebuffer holds only the opaque
             * clear colour inside the viewport rectangle. 297 draw calls and
             * 282k vertices reached the backend and none of them left a pixel.
             * That leaves exactly three causes, and they are indistinguishable
             * from the outside:
             *   behind>0 == v         -> all vertices are behind the eye
             *                            (view/camera transform wrong)
             *   behind==0, inside==0  -> outside the frustum (projection/scale)
             *   inside>0              -> clip space is fine, so the loss is in
             *                            cull / depth / render state
             * Bounded to 12 lines (this runs per submesh). */
            if (dbgNdcProbe < 12) {
                dbgNdcProbe++;
                float nx0 = 1e30f, ny0 = 1e30f, nz0 = 1e30f;
                float nx1 = -1e30f, ny1 = -1e30f, nz1 = -1e30f;
                int inside = 0, behind = 0;
                for (int i = 0; i < n; i++) {
                    float x = pos[i * 3], y = pos[i * 3 + 1], z = pos[i * 3 + 2];
                    float cx = mvp[0] * x + mvp[4] * y + mvp[8] * z + mvp[12];
                    float cy = mvp[1] * x + mvp[5] * y + mvp[9] * z + mvp[13];
                    float cz = mvp[2] * x + mvp[6] * y + mvp[10] * z + mvp[14];
                    float cw = mvp[3] * x + mvp[7] * y + mvp[11] * z + mvp[15];
                    if (cw <= 1e-5f) {
                        behind++;
                        continue;
                    }
                    float ndx = cx / cw, ndy = cy / cw, ndz = cz / cw;
                    if (ndx < nx0) nx0 = ndx;
                    if (ndx > nx1) nx1 = ndx;
                    if (ndy < ny0) ny0 = ndy;
                    if (ndy > ny1) ny1 = ndy;
                    if (ndz < nz0) nz0 = ndz;
                    if (ndz > nz1) nz1 = ndz;
                    if (ndx >= -1.0f && ndx <= 1.0f && ndy >= -1.0f
                        && ndy <= 1.0f && ndz >= -1.0f && ndz <= 1.0f) {
                        inside++;
                    }
                }
                com.sun.m3g.M3gTrace.log("ndc n=" + n + " tris=" + idx.length
                    + " behind=" + behind + " inside=" + inside
                    + " x[" + fmt(nx0) + "," + fmt(nx1) + "]"
                    + " y[" + fmt(ny0) + "," + fmt(ny1) + "]"
                    + " z[" + fmt(nz0) + "," + fmt(nz1) + "]"
                    + " cull=" + cull + " blend=" + blend
                    + " tex=" + texHandle + " vp=" + g.vpDrawW + "x"
                    + g.vpDrawH);

                /* v03.42: THE decisive line. log139/log140 both reported all 472
                 * vertices of this draw landing on ONE NDC point
                 * (x[4,4] y[-40,-40] z[98,98]) while the identical Java code,
                 * running on the host JVM against the identical 1006733-byte
                 * jar, spreads them over x[-1.477,1.440] y[-0.390,0.706]
                 * (see debug/tools/m3gprobe/HostRun). Something about the INPUT
                 * differs, and there are only two candidates: the decoded vertex
                 * array, or the model matrix. Print both raw. Emitted for the
                 * first 3 probed submeshes only. */
                if (dbgVertDump < 3) {
                    dbgVertDump++;
                    /* log142: Float.toString threw a bare RuntimeException here, so print IEEE bits (hx) instead. */
                    try {
                    com.sun.m3g.M3gTrace.log("ndc src hdr cnt=" + n
                        + " comps=" + vb.positions.numComponents
                        + " sz=" + vb.positions.componentSize
                        + " fdata=" + (vb.positions.floatData != null));
                    com.sun.m3g.M3gTrace.log("ndc src pos v0=" + hx(pos[0]) + "," + hx(pos[1]) + "," + hx(pos[2])
                        + " v1=" + hx(pos[3]) + "," + hx(pos[4]) + "," + hx(pos[5])
                        + " vL=" + hx(pos[(n - 1) * 3]) + "," + hx(pos[(n - 1) * 3 + 1])
                        + "," + hx(pos[(n - 1) * 3 + 2]));
                    com.sun.m3g.M3gTrace.log("ndc src scale ps=" + hx(vb.positionScale)
                        + " pb=" + hx(vb.positionBias[0]) + "," + hx(vb.positionBias[1])
                        + "," + hx(vb.positionBias[2]));
                    com.sun.m3g.M3gTrace.log("ndc src raw r0=" + hx(vb.positions.componentRaw(0, 0))
                        + "," + hx(vb.positions.componentRaw(0, 1)) + "," + hx(vb.positions.componentRaw(0, 2))
                        + " rL=" + hx(vb.positions.componentRaw(n - 1, 0)));
                    com.sun.m3g.M3gTrace.log("ndc src model m=" + hx(model[0]) + "," + hx(model[5])
                        + "," + hx(model[10]) + " t=" + hx(model[12]) + "," + hx(model[13])
                        + "," + hx(model[14]));
                    } catch (Throwable t) {
                        com.sun.m3g.M3gTrace.log("ndc src FAILED " + t);
                    }
                }

                /* v03.36: winding as seen in GL window space.
                 * With culling on we enable GL_CULL_FACE + GL_BACK +
                 * glFrontFace(GL_CCW). JSR-184 defines winding in a SCREEN space
                 * whose y axis points down, while GL window space is y-up, so a
                 * front face in M3G is a back face in GL - in which case every
                 * triangle of a CULL_BACK mesh is discarded and the framebuffer
                 * keeps only the clear colour, which is exactly what log137's
                 * readback probe showed (centre = the clear colour, nothing
                 * else). Measuring the sign of the projected triangle area
                 * settles it: pos==0 && neg>0 means we are culling the wrong
                 * side. */
                int posArea = 0, negArea = 0;
                for (int t = 0; t + 2 < idx.length; t += 3) {
                    float ax = ndcX(mvp, pos, idx[t]);
                    float ay = ndcY(mvp, pos, idx[t]);
                    float bx = ndcX(mvp, pos, idx[t + 1]);
                    float by = ndcY(mvp, pos, idx[t + 1]);
                    float cx2 = ndcX(mvp, pos, idx[t + 2]);
                    float cy2 = ndcY(mvp, pos, idx[t + 2]);
                    if (Float.isNaN(ax) || Float.isNaN(bx) || Float.isNaN(cx2)) {
                        continue;
                    }
                    float cross = (bx - ax) * (cy2 - ay) - (by - ay) * (cx2 - ax);
                    if (cross > 0.0f) {
                        posArea++;
                    } else if (cross < 0.0f) {
                        negArea++;
                    }
                }
                com.sun.m3g.M3gTrace.log("ndc winding posArea=" + posArea
                    + " negArea=" + negArea + " cull=" + cull);

                /* v03.38: once a few submeshes have been measured, decide once
                 * and for all. Only an unambiguous result is acted on: EVERY
                 * triangle a back face means our GL front face is the wrong
                 * one; a mixed scene means the orientation is already right and
                 * the back faces are genuinely back-facing. */
                dbgPosArea += posArea;
                dbgNegArea += negArea;
                dbgWindingSamples++;
                if (windingFix < 0 && dbgWindingSamples >= 3) {
                    windingFix = (dbgPosArea == 0 && dbgNegArea > 0) ? 1 : 0;
                    com.sun.m3g.M3gTrace.log("ndc winding FIX flip=" + windingFix
                        + " pos=" + dbgPosArea + " neg=" + dbgNegArea
                        + " samples=" + dbgWindingSamples);
                }
            }

            long ptw = System.currentTimeMillis();
            /* v04.29: bit 16 of the texture argument = the Texture2D asks for FILTER_NEAREST (the handle is 1..64)
             * v04.35: bits 17/18 = WRAP_CLAMP for S/T. */
            NativeBridge.draw(pos, (texHandle != 0) ? uv : null, rgba, idx,
                              idx.length,
                              texHandle | (texNearest << 16) | (texWrap << 17));
            ptAdd(6, ptw);
        }
    }

    /* v03.38: the correction is applied on every frame, so the reversed copy is
     * cached per index buffer (IndexBuffer.triangleList() returns a stable
     * array) instead of allocating a fresh int[] per submesh per frame. */
    private static final Object[] flipKey = new Object[32];
    private static final int[][] flipVal = new int[32][];
    private static int flipN = 0;

    private static int[] flipWinding(int[] idx) {
        for (int i = 0; i < flipN; i++) {
            if (flipKey[i] == idx) {
                return flipVal[i];
            }
        }
        int[] r = new int[idx.length];
        for (int i = 0; i + 2 < idx.length; i += 3) {
            r[i] = idx[i];
            r[i + 1] = idx[i + 2];
            r[i + 2] = idx[i + 1];
        }
        if (flipN < flipKey.length) {
            flipKey[flipN] = idx;
            flipVal[flipN] = r;
            flipN++;
        }
        return r;
    }

    /**
     * Per-vertex colour: material + lights when the mesh has normals,
     * otherwise the vertex colour (or the buffer's default colour).
     */
    private static byte[] shade(VertexBuffer vb, float[] model, Light[] lights,
                                float[][] lmats, Material mat) {
        int n = vb.numVertices;
        byte[] out = new byte[n * 4];
        VertexArray normals = vb.normals;
        VertexArray colors = vb.colors;

        float er = 0.0f, eg = 0.0f, eb = 0.0f;   /* emissive */
        float ar = 0.2f, ag = 0.2f, ab = 0.2f;   /* ambient */
        float dr = 0.8f, dg = 0.8f, db = 0.8f;   /* diffuse */
        if (mat != null) {
            int e = mat.getColor(Material.EMISSIVE);
            int a = mat.getColor(Material.AMBIENT);
            int d = mat.getColor(Material.DIFFUSE);
            er = ((e >> 16) & 0xFF) / 255.0f;
            eg = ((e >> 8) & 0xFF) / 255.0f;
            eb = (e & 0xFF) / 255.0f;
            ar = ((a >> 16) & 0xFF) / 255.0f;
            ag = ((a >> 8) & 0xFF) / 255.0f;
            ab = (a & 0xFF) / 255.0f;
            dr = ((d >> 16) & 0xFF) / 255.0f;
            dg = ((d >> 8) & 0xFF) / 255.0f;
            db = (d & 0xFF) / 255.0f;
        }

        float[] nm = new float[9];
        Matrix4.normalMatrix(nm, model);

        /* v04.10: what a light contributes that does not depend on the vertex, computed once per call instead of once
         * per vertex (log177: shade is ~450 ms/frame). Same expressions, same accumulation order below. */
        int nl = lights.length;
        boolean[] lOn = new boolean[nl];
        int[] lMode = new int[nl];
        float[] lR = new float[nl], lG = new float[nl], lB = new float[nl];
        float[] lX = new float[nl], lY = new float[nl], lZ = new float[nl];   /* direction, or position of a point light */
        float[] lLen = new float[nl];                                         /* length of the direction */
        float[] lA0 = new float[nl], lA1 = new float[nl], lA2 = new float[nl];
        boolean needPos = false;
        for (int i = 0; i < nl; i++) {
            Light lt = lights[i];
            if (lt == null) {
                continue;
            }
            lOn[i] = true;
            float inten = lt.getIntensity();
            int lc = lt.getColor();
            lR[i] = ((lc >> 16) & 0xFF) / 255.0f * inten;
            lG[i] = ((lc >> 8) & 0xFF) / 255.0f * inten;
            lB[i] = (lc & 0xFF) / 255.0f * inten;
            lMode[i] = lt.getMode();
            if (lMode[i] == Light.AMBIENT) {
                continue;
            }
            float[] lm = lmats[i];
            if (lMode[i] == Light.DIRECTIONAL) {
                /* light direction is the node's -Z axis */
                lX[i] = -lm[8];
                lY[i] = -lm[9];
                lZ[i] = -lm[10];
                lLen[i] = (float)Math.sqrt(lX[i] * lX[i] + lY[i] * lY[i] + lZ[i] * lZ[i]);
            } else {
                needPos = true;
                lX[i] = lm[12];
                lY[i] = lm[13];
                lZ[i] = lm[14];
                float[] att = new float[3];
                lt.getAttenuation(att);
                lA0[i] = att[0];
                lA1[i] = att[1];
                lA2[i] = att[2];
            }
        }
        boolean colorA = colors != null && colors.numComponents > 3;

        /* v04.10: does anything actually light this mesh?
         *
         * Lights only ever reach shade() from render(World) (a World's own
         * Light nodes) or from Graphics3D.addLight(). A title that renders
         * every model with render(Node, Transform) - which is exactly what
         * 极品飞车 does - gets NEITHER unless it also calls addLight().
         * With the list empty the per-vertex sum below adds nothing and the
         * vertex ends up as `emissive + ambient*0.2`. For a mesh whose
         * Appearance has no Material (log179: /aq's 24 Appearances all have
         * material == null and texture == null) that is 0 + 0.2*0.2 = 0.04,
         * so the car came out essentially black even though the geometry was
         * provably on screen (inside=609/698, 19050 verts/frame).
         *
         * The fallback below is applied ONLY when no light with a direction
         * survives, so every scene that does supply lights keeps its exact
         * current output. */
        boolean anyDirect = false;
        for (int i = 0; i < nl; i++) {
            if (lOn[i] && lMode[i] != Light.AMBIENT) {
                anyDirect = true;
                break;
            }
        }

        /* v04.20: with only ambient / directional lights the whole loop below runs natively (m3g_shade.h). */
        if (useNativeShade && !needPos) {
            float[] prm = new float[22 + 9 * nl];
            prm[0] = er; prm[1] = eg; prm[2] = eb;
            prm[3] = ar; prm[4] = ag; prm[5] = ab;
            prm[6] = dr; prm[7] = dg; prm[8] = db;
            System.arraycopy(nm, 0, prm, 9, 9);
            System.arraycopy(vb.defaultColor, 0, prm, 18, 4);
            for (int i = 0; i < nl; i++) {
                int o = 22 + 9 * i;
                prm[o] = lOn[i] ? 1.0f : 0.0f;
                prm[o + 1] = (lMode[i] == Light.AMBIENT) ? 0.0f : 1.0f;
                prm[o + 2] = lR[i]; prm[o + 3] = lG[i]; prm[o + 4] = lB[i];
                prm[o + 5] = lX[i]; prm[o + 6] = lY[i]; prm[o + 7] = lZ[i];
                prm[o + 8] = lLen[i];
            }
            int[] nI = null, cI = null;
            float[] nF = null, cF = null;
            int nC = 0, nS = 0, cC = 0, cS = 0;
            if (normals != null) {
                nC = normals.numComponents;
                nS = normals.componentSize;
                if (normals.floatData != null) nF = normals.floatData; else nI = normals.data;
            }
            if (colors != null) {
                cC = colors.numComponents;
                cS = colors.componentSize;
                if (colors.floatData != null) cF = colors.floatData; else cI = colors.data;
            }
            if (NativeBridge.shade(out, n, prm, nI, nF, nC, nS, cI, cF, cC, cS, nl)) {
                shadeNativeN++;
                return out;
            }
            useNativeShade = false;
            com.sun.m3g.M3gTrace.log("native shade refused; using the Java loop");
        }
        shadeJavaN++;

        for (int v = 0; v < n; v++) {
            float r = er, gg = eg, b = eb;
            float nx = 0.0f, ny = 0.0f, nz = 1.0f;
            if (normals != null) {
                float lx = normals.component(v, 0);
                float ly = normals.component(v, 1);
                float lz = normals.component(v, 2);
                nx = nm[0] * lx + nm[3] * ly + nm[6] * lz;
                ny = nm[1] * lx + nm[4] * ly + nm[7] * lz;
                nz = nm[2] * lx + nm[5] * ly + nm[8] * lz;
                float len = (float)Math.sqrt(nx * nx + ny * ny + nz * nz);
                if (len > 0.0f) {
                    nx /= len;
                    ny /= len;
                    nz /= len;
                }
            }
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            if (normals != null && needPos) {
                float rx = vb.positions.componentRaw(v, 0);
                float ry = vb.positions.componentRaw(v, 1);
                float rz = vb.positions.componentRaw(v, 2);
                px = model[0] * rx + model[4] * ry + model[8] * rz + model[12];
                py = model[1] * rx + model[5] * ry + model[9] * rz + model[13];
                pz = model[2] * rx + model[6] * ry + model[10] * rz + model[14];
            }
            r += ar * 0.2f;
            gg += ag * 0.2f;
            b += ab * 0.2f;

            /* v04.10: the unlit fallback (see anyDirect above). A fixed
             * world-space key light from the front-upper-left - a pure
             * function of the vertex normal, so it cannot depend on camera
             * state and is identical on both backends. The 0.25 floor keeps
             * faces pointing away from it readable instead of pitch black. */
            if (!anyDirect) {
                float ndl = nx * 0.3216f + ny * 0.6432f + nz * 0.7504f;
                float k = (ndl > 0.0f) ? (0.25f + 0.75f * ndl) : 0.25f;
                r += dr * k;
                gg += dg * k;
                b += db * k;
            }

            for (int i = 0; i < nl; i++) {
                if (!lOn[i]) {
                    continue;
                }
                float lr = lR[i];
                float lg = lG[i];
                float lb = lB[i];
                int mode = lMode[i];
                if (mode == Light.AMBIENT) {
                    r += ar * lr;
                    gg += ag * lg;
                    b += ab * lb;
                    continue;
                }
                float lvx, lvy, lvz, atten = 1.0f, ll;
                if (mode == Light.DIRECTIONAL) {
                    lvx = lX[i];
                    lvy = lY[i];
                    lvz = lZ[i];
                    ll = lLen[i];
                } else {
                    lvx = lX[i] - px;
                    lvy = lY[i] - py;
                    lvz = lZ[i] - pz;
                    float d2 = lvx * lvx + lvy * lvy + lvz * lvz;
                    float d = (float)Math.sqrt(d2);
                    atten = 1.0f / (lA0[i] + lA1[i] * d + lA2[i] * d2);
                    if (atten > 1.0f) {
                        atten = 1.0f;
                    }
                    ll = d;     /* the same expression the length used to be recomputed from */
                }
                if (ll <= 0.0f) {
                    continue;
                }
                float ndl = (nx * lvx + ny * lvy + nz * lvz) / ll;
                if (ndl <= 0.0f) {
                    continue;
                }
                ndl *= atten;
                r += dr * lr * ndl;
                gg += dg * lg * ndl;
                b += db * lb * ndl;
            }

            float cr = 1.0f, cg = 1.0f, cb = 1.0f, ca = 1.0f;
            if (colors != null) {
                cr = colors.component(v, 0);
                cg = colors.component(v, 1);
                cb = colors.component(v, 2);
                if (colorA) {
                    ca = colors.component(v, 3);
                }
            } else {
                cr = vb.defaultColor[0];
                cg = vb.defaultColor[1];
                cb = vb.defaultColor[2];
                ca = vb.defaultColor[3];
            }
            out[v * 4 + 0] = clamp(r * cr);
            out[v * 4 + 1] = clamp(gg * cg);
            out[v * 4 + 2] = clamp(b * cb);
            out[v * 4 + 3] = clamp(ca);
        }
        return out;
    }

    private static byte clamp(float v) {
        int i = (int)(v * 255.0f + 0.5f);
        if (i < 0) {
            return 0;
        }
        if (i > 255) {
            return (byte)255;
        }
        return (byte)i;
    }

    // -------------------------------------------------------------- sprite

    private static void drawSprite(Graphics3D g, Sprite3D s, float[] model,
                                   float[] view, float[] proj) {
        Image2D img = s.getImage();
        if (img == null) {
            return;
        }
        int texHandle = img.getTextureHandle();
        if (texHandle == 0) {
            return;
        }
        /* Billboard: camera right/up are the rows of the view rotation. */
        float rx = view[0], ry = view[4], rz = view[8];
        float ux = view[1], uy = view[5], uz = view[9];
        float cx = model[12], cy = model[13], cz = model[14];
        float sx = model[0];
        float sy = model[5];
        if (sx == 0.0f) {
            sx = 1.0f;
        }
        if (sy == 0.0f) {
            sy = 1.0f;
        }
        float hw = 0.5f * sx * (s.isScaled() ? img.getWidth() : 1.0f);
        float hh = 0.5f * sy * (s.isScaled() ? img.getHeight() : 1.0f);

        float[] pos = new float[18];
        float[] uv = new float[12];
        float[][] corners = {
            { -hw, -hh }, { hw, -hh }, { -hw, hh },
            { hw, -hh }, { hw, hh }, { -hw, hh }
        };
        for (int i = 0; i < 6; i++) {
            float a = corners[i][0];
            float b = corners[i][1];
            pos[i * 3 + 0] = cx + rx * a + ux * b;
            pos[i * 3 + 1] = cy + ry * a + uy * b;
            pos[i * 3 + 2] = cz + rz * a + uz * b;
        }
        uv[0] = 0; uv[1] = 1; uv[2] = 1; uv[3] = 1; uv[4] = 0; uv[5] = 0;
        uv[6] = 1; uv[7] = 1; uv[8] = 1; uv[9] = 0; uv[10] = 0; uv[11] = 0;
        byte[] rgba = new byte[24];
        for (int i = 0; i < 24; i++) {
            rgba[i] = (byte)255;
        }

        float[] mvp = new float[16];
        Matrix4.mul(mvp, view, model);
        Matrix4.mul(mvp, proj, mvp);
        NativeBridge.setMvp(mvp);
        /* v04.02: sprites blend (they are billboards with alpha edges) and
         * never write depth, which is what the software path already did
         * implicitly via its old blend flag. */
        NativeBridge.setState(false, CompositingMode.ALPHA, true, false, true,
                              0.0f, 0);
        NativeBridge.draw(pos, uv, rgba, null, 6, texHandle);
    }

    // -------------------------------------------------------------- camera

    private static float[] worldMatrix(Node n) {
        float[] m = new float[16];
        n.getWorldMatrix(m);
        return m;
    }

    private static float[] projection(Camera cam, int vw, int vh) {
        float aspectView = (float)vw / (float)vh;
        int type = cam.getProjection((float[])null);
        if (type == Camera.GENERIC) {
            Transform t = new Transform();
            cam.getProjection(t);
            float[] m = new float[16];
            System.arraycopy(t.m, 0, m, 0, 16);
            return m;
        }
        float[] p = new float[4];
        cam.getProjection(p);
        /* The viewport aspect overrides the one stored on the camera: JSR-184
         * says the rendered image stretches to fill the viewport, and titles do
         * not keep camera.aspect in sync with their viewport rectangle. */
        return buildProjection(type, p[0], aspectView, p[2], p[3], null);
    }

    /**
     * Builds a projection matrix from camera parameters.
     *
     * This is the ONE definition of the matrix. Camera.getProjection(Transform)
     * calls it too, so a title that asks the camera for its matrix and one that
     * lets us render with it can never disagree - which is what JSR-184 means
     * by "parallel and perspective projections can always be returned in the
     * 4x4 matrix form".
     *
     * @param type        GENERIC (returns `generic` unchanged), PERSPECTIVE or
     *                    PARALLEL
     * @param fovy        vertical field of view in DEGREES for PERSPECTIVE, the
     *                    view volume height for PARALLEL
     * @param aspectRatio width / height of the viewport
     * @param near        near clipping distance
     * @param far         far clipping distance
     * @param generic     the stored 4x4 matrix, used only for GENERIC
     */
    static float[] buildProjection(int type, float fovy, float aspectRatio,
                                   float near, float far, float[] generic) {
        float[] m = new float[16];
        if (type == Camera.GENERIC) {
            if (generic != null) {
                System.arraycopy(generic, 0, m, 0, 16);
            } else {
                Matrix4.identity(m);
            }
            return m;
        }
        if (aspectRatio == 0.0f) {
            aspectRatio = 1.0f;
        }
        /* v03.30: fovy is in DEGREES (JSR-184 Camera.setPerspective /
         * setParallel both say "vertical field of view angle in degrees"; the
         * class default is 45.0f and this title passes 70.0f, which only makes
         * sense as degrees). We used Math.tan(fovy * 0.5f) directly, i.e.
         * interpreted it as RADIANS: for fovy = 70 that computes
         * tan(35 rad) = tan(35 - 11*PI) = tan(0.4425) = 0.476 -> f = 2.10
         * instead of the correct 1/tan(35 deg) = 1.428. Every projection was
         * therefore 1.47x too wide and the value is not even monotonic in the
         * requested angle (tan wraps every PI), so different titles got
         * arbitrary fields of view.
         * The same conversion applies to the parallel branch, where
         * fovy/2 defines the half-height of the view box. */
        float halfRad = fovy * 0.5f * (float)(Math.PI / 180.0);
        float f = 1.0f / (float)Math.tan(halfRad);
        if (type == Camera.PERSPECTIVE) {
            m[0] = f / aspectRatio;
            m[5] = f;
            m[10] = (far + near) / (near - far);
            m[11] = -1.0f;
            m[14] = (2.0f * far * near) / (near - far);
            m[15] = 0.0f;
        } else { /* PARALLEL */
            float scale = (float)Math.tan(halfRad);
            m[0] = 1.0f / (scale * aspectRatio);
            m[5] = 1.0f / scale;
            m[10] = -2.0f / (far - near);
            m[14] = -(far + near) / (far - near);
        }
        return m;
    }
}
