package javax.microedition.m3g;

import com.sun.m3g.NativeBridge;

/**
 * The 3D rendering context. One instance per MIDlet, obtained from
 * getInstance(); bindTarget opens a frame, releaseTarget submits it.
 *
 * The frame is rendered into an int[] and handed to the MIDP Graphics with
 * drawRGB, which is why no native pixel unpacking is needed.
 */
public class Graphics3D extends Object3D {

    public static final int ANTIALIAS = 2;
    public static final int DITHER = 4;
    public static final int TRUE_COLOR = 8;
    public static final int OVERWRITE = 16;

    private static final Graphics3D INSTANCE = new Graphics3D();

    javax.microedition.lcdui.Graphics target = null;
    Image2D imageTarget = null;
    int[] frame = null;
    int vpX, vpY, vpW, vpH;      /* bound target rectangle */
    int vpOffsetX, vpOffsetY;    /* viewport inside it */
    int vpDrawW, vpDrawH;
    private Camera camera = null;
    private final Transform cameraTransform = new Transform();
    private boolean cameraFromTransform = false;
    private boolean bound = false;
    private boolean reportedTarget = false;
    private String tid = "";
    private int tW = -1, tH = -1;
    private int bindCount = 0;
    private static final int VP_SEEN_MAX = 8;
    private static final int[][] vpSeen = new int[VP_SEEN_MAX][4];
    private static int vpSeenN = 0;
    private int backendRc = 0;
    private int initW = 0, initH = 0;

    private Graphics3D() {
    }

    private static boolean jitFpChecked = false;

    public static Graphics3D getInstance() {
        if (!jitFpChecked) {
            jitFpChecked = true;
            com.sun.m3g.JitSelfTest.runOnce();   /* v04.51 diagnostic, ~1s once per process round */
        }
        return INSTANCE;
    }

    /** JSR 184 1.1: depth buffer on, default hints. */
    public void bindTarget(Object target) {
        bindTarget(target, true, 0);
    }

    /**
     * Restricts drawing to a sub-rectangle of the bound target.
     * Coordinates are in target pixels; the projection is adjusted through
     * the viewport aspect in Renderer.
     */
    public void setViewport(int x, int y, int width, int height) {
        if (width < 0 || height < 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative viewport size"));
        }
        if (!bound) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalStateException("no bound target"));
        }
        /* Clamp instead of throwing: titles routinely pass a viewport sized
         * for a different handset (the CAPCOM title asks for 240x180 on a
         * 176-wide canvas). A clamped viewport still renders the scene,
         * whereas an exception aborts the whole paint and leaves the screen
         * black. */
        if (width <= 0 || height <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("empty viewport"));
        }
        if (x + width > vpW || y + height > vpH) {
            com.sun.m3g.M3gTrace.log("viewport clamp " + x + "," + y + " "
                                     + width + "x" + height
                                     + " -> target " + vpW + "x" + vpH);
        }
        if (x < 0) { width += x; x = 0; }
        if (y < 0) { height += y; y = 0; }
        if (x + width > vpW) { width = vpW - x; }
        if (y + height > vpH) { height = vpH - y; }
        if (width <= 0 || height <= 0) {
            /* Entirely outside the target: draw nothing rather than throw.
             * Throwing aborts the caller's paint and black-screens the
             * frame, which is far worse than an empty viewport. */
            vpDrawW = 0;
            vpDrawH = 0;
            NativeBridge.setScissor(0, 0, 0, 0);
            return;
        }
        vpOffsetX = x;
        vpOffsetY = y;
        vpDrawW = width;
        vpDrawH = height;
        NativeBridge.setScissor(x, y, width, height);
        /* v03.30: record every DISTINCT viewport rectangle (bounded to 8).
         * log135 showed only "vp=0,105 240x140", which is BioCtrl.draw3DMap's
         * rect; the gameplay rect is computed from fields, so there was no way
         * to tell whether the gameplay path ever set a viewport at all. Seeing
         * the distinct rects makes that answerable in one run. */
        if (vpSeenN < VP_SEEN_MAX) {
            boolean dup = false;
            for (int i = 0; i < vpSeenN; i++) {
                if (vpSeen[i][0] == x && vpSeen[i][1] == y
                    && vpSeen[i][2] == width && vpSeen[i][3] == height) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                vpSeen[vpSeenN][0] = x;
                vpSeen[vpSeenN][1] = y;
                vpSeen[vpSeenN][2] = width;
                vpSeen[vpSeenN][3] = height;
                vpSeenN++;
                com.sun.m3g.M3gTrace.log("viewport " + x + "," + y + " "
                                         + width + "x" + height
                                         + " (#" + vpSeenN + ")");
            }
        }
    }

    public int getViewportX() {
        return vpOffsetX;
    }

    public int getViewportY() {
        return vpOffsetY;
    }

    public int getViewportWidth() {
        return vpDrawW;
    }

    public int getViewportHeight() {
        return vpDrawH;
    }

    public void bindTarget(Object target, boolean depthBuffer, int hints) {
        if (target == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        /* A title that lets an exception escape between bindTarget and
         * releaseTarget would otherwise be locked out forever: the flag
         * stays set and every later frame fails. Treat a re-bind as an
         * implicit release of the previous target, which is what the game
         * intended. */
        if (bound) {
            releaseTarget();
        }
        if (target instanceof javax.microedition.lcdui.Graphics) {
            this.target = (javax.microedition.lcdui.Graphics)target;
            this.imageTarget = null;
            /* v03.18: the M3G target is the whole drawing surface, NOT the
             * caller's clip rectangle. Viewport coordinates are relative to
             * the surface's top-left corner, so using the clip here truncates
             * any viewport that the title places inside its own clip.
             * log127 made this concrete: the title does
             *   g.setClip(0, 40, 240, 180); g3d.bindTarget(g);
             *   g3d.setViewport(0, 40, 240, 180);
             * With the clip as the target (240x180) the requested 0,40 240x180
             * looked out of range and got clamped to 140 pixels tall, so the
             * 3D scene was squeezed into the wrong rectangle.
            /* Graphics exposes no size accessor. Ask the port's display layer
             * first (NativeBridge.screenWidth/Height) - in this port the LCDUI
             * canvas covers exactly the virtual screen. The Displayable route
             * was tried in v03.18 and never worked: Display.getDisplay(null)
             * throws in this MIDP because it needs a MIDlet instance, so the
             * clip (240x180 here) was used and the viewport got clamped.
             * Keep the clip as the last resort for a surface of unknown size. */
            int dw = this.target.getClipWidth();
            int dh = this.target.getClipHeight();
            int sw = com.sun.m3g.NativeBridge.screenWidth();
            int sh = com.sun.m3g.NativeBridge.screenHeight();
            if (sw > 0 && sh > 0) {
                dw = sw;
                dh = sh;
            }
            /* vpX/vpY are the origin of the target inside the destination of
             * drawRGB. They must stay 0: the viewport offset is already
             * relative to the surface, and adding the clip origin here would
             * shift the blit twice (log126/log127 asked for y=40 with
             * clipY=40, which would have landed at 80). */
            vpX = 0;
            vpY = 0;
            vpW = dw;
            vpH = dh;
        } else if (target instanceof Image2D) {
            this.imageTarget = (Image2D)target;
            this.target = null;
            vpX = 0;
            vpY = 0;
            vpW = imageTarget.getWidth();
            vpH = imageTarget.getHeight();
        } else {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("target must be Graphics or Image2D"));
        }
        if (vpW <= 0 || vpH <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("empty viewport"));
        }
        /* v03.30: report every DISTINCT target, not just the first one.
         * log135 printed "bind target 240x320" once and then 5 render lines with
         * vp=0,105 240x140, and nothing at all afterwards even though a mission
         * started (its 34 s BGM played). With a one-shot flag we cannot tell
         * whether gameplay bound a different target (e.g. an Image2D) or never
         * bound at all. */
        String kind = (this.target != null) ? "g" : "img";
        if (!reportedTarget || tid != kind
            || tW != vpW || tH != vpH) {
            reportedTarget = true;
            tid = kind;
            tW = vpW;
            tH = vpH;
            com.sun.m3g.M3gTrace.log("bind target " + vpW + "x" + vpH
                                     + " kind=" + kind
                                     + " clip=" + vpX + "," + vpY);
        }
        if (frame == null || frame.length < vpW * vpH) {
            frame = new int[vpW * vpH];
        }
        vpOffsetX = 0;
        vpOffsetY = 0;
        vpDrawW = vpW;
        vpDrawH = vpH;
        /* v03.25: initialise the backend only when the target changed or the
         * previous attempt failed. This used to run on EVERY bindTarget, i.e.
         * once per frame; with a broken backend that meant a malloc attempt
         * per frame plus two breadcrumb lines each time (log131 has dozens of
         * "3d: hardware init failed ... software init failed" pairs), and it
         * re-mallocated the native frame buffer repeatedly. */
        if (backendRc <= 0 || vpW != initW || vpH != initH) {
            backendRc = NativeBridge.init(vpW, vpH);
            initW = vpW;
            initH = vpH;
            com.sun.m3g.M3gTrace.log("backend init rc=" + backendRc
                                     + " active=" + NativeBridge.backend()
                                     + " target=" + vpW + "x" + vpH);
        }
        NativeBridge.setScissor(0, 0, vpW, vpH);
        bound = true;
        /* v03.30: bindTarget counter. log135 rendered 5 frames (the mission map)
         * and then nothing for the rest of the session even though a mission
         * started and its 34 s BGM played - with the render report firing only
         * every 120 releases, "gameplay never bound a target" and "gameplay
         * rendered <120 frames" looked identical. Counting binds makes the
         * difference visible without flooding the log. */
        Renderer.ptBindAt = System.currentTimeMillis();
        if (Renderer.ptReleaseAt != 0) {
            Renderer.ptMs[1] += Renderer.ptBindAt - Renderer.ptReleaseAt;
        }
        bindCount++;
        if (bindCount <= 3 || bindCount % 300 == 0) {
            com.sun.m3g.M3gTrace.log("bind #" + bindCount + " "
                                     + vpW + "x" + vpH
                                     + " kind=" + ((target != null) ? "g" : "img"));
        }
    }

    public void releaseTarget() {
        if (!bound) {
            return;
        }
        /* Advance the animation clock by the time this frame took, measured
         * rather than assumed so a slow device plays animation slower
         * instead of skipping keyframes. */
        long ptr0 = System.currentTimeMillis();
        long ptSplit = 0;
        Renderer.advanceWorldTime();
        long ptp = System.currentTimeMillis();
        NativeBridge.present(frame, vpW, vpH);
        ptSplit += System.currentTimeMillis() - ptp;
        Renderer.ptAdd(7, ptp);
        /* v03.22: bounded render statistics (every 30 frames). A blank frame
         * with no exception was undiagnosable from the device log; this says
         * whether the renderer ran, how many meshes it visited, and whether
         * the camera/viewport were sane. */
        Renderer.reportFrameStats(this);
        if (target != null) {
            /* Only the viewport rectangle was drawn; blit just that. */
            long ptb = System.currentTimeMillis();
            target.drawRGB(frame, vpOffsetY * vpW + vpOffsetX, vpW,
                           vpX + vpOffsetX, vpY + vpOffsetY,
                           vpDrawW, vpDrawH, true);
            ptSplit += System.currentTimeMillis() - ptb;
            Renderer.ptAdd(8, ptb);
        } else if (imageTarget != null) {
            byte[] px = new byte[vpW * vpH * 4];
            int o = 0;
            for (int i = 0; i < vpW * vpH; i++) {
                int p = frame[i];
                px[o++] = (byte)((p >> 16) & 0xFF);
                px[o++] = (byte)((p >> 8) & 0xFF);
                px[o++] = (byte)(p & 0xFF);
                px[o++] = (byte)((p >> 24) & 0xFF);
            }
            imageTarget.set(0, 0, vpW, vpH, px);
        }
        Renderer.ptReleaseAt = System.currentTimeMillis();
        Renderer.ptMs[20] += (Renderer.ptReleaseAt - ptr0) - ptSplit;   /* releaseTarget minus present and blit */
        Renderer.ptMs[0] += Renderer.ptReleaseAt - Renderer.ptBindAt;
        Renderer.ptReport();
        target = null;
        imageTarget = null;
        bound = false;
    }

    public void setCamera(Camera camera, Transform transform) {
        this.camera = camera;
        if (transform == null) {
            cameraFromTransform = false;
        } else {
            cameraTransform.set(transform);
            cameraFromTransform = true;
        }
    }

    /* v03.22: package-private accessors for Renderer. renderWorld() must use
     * the camera set with setCamera(camera, transform) - JSR-184 says that is
     * the camera rendering uses, with the World's active camera only as a
     * fallback - and the transform given there REPLACES the camera node's own
     * transform. The CAPCOM title only ever calls setCamera(), so ignoring
     * these put the eye at the world origin and the frame came out empty. */
    Camera cameraInternal() {
        return camera;
    }

    boolean cameraFromTransformInternal() {
        return cameraFromTransform;
    }

    Transform cameraTransformInternal() {
        return cameraTransform;
    }

    public Camera getCamera(Transform transform) {
        if (camera != null && transform != null) {
            if (cameraFromTransform) {
                transform.set(cameraTransform);
            } else {
                camera.getTransform(transform);
            }
        }
        return camera;
    }

    public void render(World world) {
        long t0 = System.currentTimeMillis();
        Renderer.renderWorld(this, world);
        Renderer.ptAdd(16, t0);
    }

    public void render(Node node, Transform transform) {
        long t0 = System.currentTimeMillis();
        Renderer.renderNode(this, node, transform);
        Renderer.ptAdd(16, t0);
    }

    public void clear(Background background) {
        long t0 = System.currentTimeMillis();
        Renderer.clear(this, background);
        Renderer.ptAdd(17, t0);
    }

    public void setDepthRange(float near, float far) {
        if (near < 0.0f || far > 1.0f || near > far) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad depth range"));
        }
    }

    public void resetLights() {
        long t0 = System.currentTimeMillis();
        Renderer.resetLights();
        Renderer.ptAdd(18, t0);
    }

    public int addLight(Light light, Transform transform) {
        long t0 = System.currentTimeMillis();
        int r = Renderer.addLight(light, transform);
        Renderer.ptAdd(18, t0);
        return r;
    }

    public void setLight(int index, Light light, Transform transform) {
        long t0 = System.currentTimeMillis();
        Renderer.setLight(index, light, transform);
        Renderer.ptAdd(18, t0);
    }

    boolean isBound() {
        return bound;
    }
}
