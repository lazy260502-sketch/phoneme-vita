import javax.microedition.lcdui.Canvas;
import javax.microedition.lcdui.Display;
import javax.microedition.lcdui.Graphics;
import javax.microedition.lcdui.Image;
import javax.microedition.m3g.Appearance;
import javax.microedition.m3g.Background;
import javax.microedition.m3g.Camera;
import javax.microedition.m3g.Graphics3D;
import javax.microedition.m3g.Image2D;
import javax.microedition.m3g.Light;
import javax.microedition.m3g.Material;
import javax.microedition.m3g.Mesh;
import javax.microedition.m3g.PolygonMode;
import javax.microedition.m3g.Texture2D;
import javax.microedition.m3g.Transform;
import javax.microedition.m3g.TriangleStripArray;
import javax.microedition.m3g.VertexArray;
import javax.microedition.m3g.VertexBuffer;
import javax.microedition.m3g.World;
import javax.microedition.midlet.MIDlet;

/**
 * M3G smoke test for the Vita port: a textured, lit, rotating cube.
 *
 * It exercises the whole chain - JSR 184 classes -> Renderer -> KNI ->
 * vita_3d backend (hardware vitaGL or software rasteriser, chosen in
 * 设置 > 3D 模式) -> Graphics.drawRGB.
 */
public class M3GTest extends MIDlet implements Runnable {

    private M3GCanvas canvas;

    protected void startApp() {
        if (canvas == null) {
            canvas = new M3GCanvas();
            new Thread(this).start();
        }
        Display.getDisplay(this).setCurrent(canvas);
    }

    protected void pauseApp() {
    }

    protected void destroyApp(boolean unconditional) {
    }

    public void run() {
        while (true) {
            canvas.tick();
            try {
                Thread.sleep(33);
            } catch (InterruptedException e) {
                return;
            }
        }
    }
}

class M3GCanvas extends Canvas {

    private final World world;
    private final Camera camera;
    private final Mesh cube;
    private final Transform cubeTransform = new Transform();
    private int angle = 0;

    M3GCanvas() {
        world = new World();

        camera = new Camera();
        /* vertical fov, aspect is replaced by the viewport's own ratio */
        camera.setPerspective(45.0f, 1.0f, 0.1f, 100.0f);
        camera.setTranslation(0.0f, 0.0f, 5.0f);
        world.addChild(camera);
        world.setActiveCamera(camera);

        Background bg = new Background();
        bg.setColor(0xFF202040);
        world.setBackground(bg);

        Light light = new Light();
        light.setMode(Light.DIRECTIONAL);
        light.setColor(0x00FFFFFF);
        light.setIntensity(1.0f);
        light.setTranslation(1.0f, 2.0f, 3.0f);
        world.addChild(light);
        world.addLight(light);

        cube = buildCube();
        cube.setTransform(cubeTransform);
        world.addChild(cube);
    }

    private Mesh buildCube() {
        float[] pos = {
            /* +Z */ -1, -1,  1,   1, -1,  1,   1,  1,  1,  -1,  1,  1,
            /* -Z */  1, -1, -1,  -1, -1, -1,  -1,  1, -1,   1,  1, -1,
            /* +X */  1, -1,  1,   1, -1, -1,   1,  1, -1,   1,  1,  1,
            /* -X */ -1, -1, -1,  -1, -1,  1,  -1,  1,  1,  -1,  1, -1,
            /* +Y */ -1,  1,  1,   1,  1,  1,   1,  1, -1,  -1,  1, -1,
            /* -Y */ -1, -1, -1,   1, -1, -1,   1, -1,  1,  -1, -1,  1
        };
        float[] nrm = {
            0, 0, 1,  0, 0, 1,  0, 0, 1,  0, 0, 1,
            0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1,
            1, 0, 0,  1, 0, 0,  1, 0, 0,  1, 0, 0,
            -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0,
            0, 1, 0,  0, 1, 0,  0, 1, 0,  0, 1, 0,
            0, -1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0
        };
        float[] uv = {
            0, 0,  1, 0,  1, 1,  0, 1,
            0, 0,  1, 0,  1, 1,  0, 1,
            0, 0,  1, 0,  1, 1,  0, 1,
            0, 0,  1, 0,  1, 1,  0, 1,
            0, 0,  1, 0,  1, 1,  0, 1,
            0, 0,  1, 0,  1, 1,  0, 1
        };
        /* two triangles per face, 6 faces */
        int[] idx = new int[36];
        int o = 0;
        for (int f = 0; f < 6; f++) {
            int b = f * 4;
            idx[o++] = b;     idx[o++] = b + 1; idx[o++] = b + 2;
            idx[o++] = b;     idx[o++] = b + 2; idx[o++] = b + 3;
        }

        VertexBuffer vb = new VertexBuffer(24, 3, VertexArray.FLOAT, 3,
                                           VertexArray.FLOAT, 0,
                                           VertexArray.BYTE, 2,
                                           VertexArray.FLOAT);
        VertexArray vpos = new VertexArray(24, 3, VertexArray.FLOAT);
        vpos.set(0, 24, pos);
        VertexArray vnrm = new VertexArray(24, 3, VertexArray.FLOAT);
        vnrm.set(0, 24, nrm);
        VertexArray vuv = new VertexArray(24, 2, VertexArray.FLOAT);
        vuv.set(0, 24, uv);
        vb.setPositions(vpos);
        vb.setNormals(vnrm);
        vb.setTexCoords(0, vuv, null);

        /* One strip per triangle: a single 36-index strip would flip the
         * winding on every other triangle. */
        TriangleStripArray ib = new TriangleStripArray(idx, triangles(12));

        Appearance ap = new Appearance();
        Material mat = new Material();
        mat.setColor(Material.DIFFUSE, 0x00FFFFFF);
        mat.setColor(Material.AMBIENT, 0x00404040);
        ap.setMaterial(mat);

        PolygonMode pm = new PolygonMode();
        pm.setCulling(PolygonMode.CULL_BACK);
        pm.setWinding(PolygonMode.WINDING_CCW);
        ap.setPolygonMode(pm);

        Texture2D tex = new Texture2D(new Image2D(Image2D.RGB, makeChecker()));
        tex.setFiltering(Texture2D.FILTER_BASE_LEVEL, Texture2D.FILTER_NEAREST);
        ap.setTexture(0, tex);

        return new Mesh(vb, ib, ap);
    }

    /** n triangles of 3 indices each */
    private static int[] triangles(int n) {
        int[] r = new int[n];
        for (int i = 0; i < n; i++) {
            r[i] = 3;
        }
        return r;
    }

    private static Image makeChecker() {
        Image img = Image.createImage(16, 16);
        Graphics g = img.getGraphics();
        g.setColor(0xFFFFFF);
        g.fillRect(0, 0, 16, 16);
        g.setColor(0x3060C0);
        for (int y = 0; y < 16; y += 8) {
            for (int x = 0; x < 16; x += 8) {
                if (((x ^ y) & 8) != 0) {
                    g.fillRect(x, y, 8, 8);
                }
            }
        }
        return img;
    }

    void tick() {
        angle = (angle + 3) % 360;
        repaint();
    }

    protected void paint(Graphics g) {
        g.setColor(0x000000);
        g.fillRect(0, 0, getWidth(), getHeight());

        Graphics3D g3d = Graphics3D.getInstance();
        try {
            g3d.bindTarget(g, true, Graphics3D.TRUE_COLOR);
            cubeTransform.setIdentity();
            cubeTransform.postRotate((float)angle, 0.0f, 1.0f, 0.0f);
            cubeTransform.postRotate((float)angle * 0.5f, 1.0f, 0.0f, 0.0f);
            g3d.render(world);
        } finally {
            g3d.releaseTarget();
        }
        g.setColor(0xFFFFFF);
        g.drawString("M3G " + angle + " " + getWidth() + "x" + getHeight(),
                     4, 4, Graphics.TOP | Graphics.LEFT);
    }

    protected void keyPressed(int key) {
        /* nothing: the cube spins on its own */
    }
}
