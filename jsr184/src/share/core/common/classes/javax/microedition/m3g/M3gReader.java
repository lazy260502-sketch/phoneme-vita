package javax.microedition.m3g;

import java.io.IOException;

import com.sun.m3g.M3gFile;

/**
 * Turns an M3gFile object table into live M3G objects.
 *
 * Field layouts follow the JSR 184 file format; every one of them was
 * verified byte-for-byte against the 15 .m3g files in debug/3d_game by
 * requiring each object body to be consumed exactly. A mismatch raises
 * IOException instead of silently producing a broken scene.
 *
 * Two passes because references are object userIDs that may point forward:
 *   1. create an empty instance per object slot, indexed by userID
 *   2. parse each body and resolve references against that table
 *
 * Package-private on purpose: it writes the fields of the API classes
 * directly instead of widening their public surface.
 */
final class M3gReader {

    private final byte[] data;
    private final M3gFile file;
    private final Object3D[] byId;
    private final int[] slotType;
    private final int[] slotOffset;
    private final int[] slotLength;
    private final Object3D[] slotObject;

    /** External resources that could not be resolved (diagnostics). */
    static int missedImages = 0;

    private M3gReader(byte[] data, M3gFile file) {
        this.data = data;
        this.file = file;
        int n = file.objects.length;
        slotType = new int[n];
        slotOffset = new int[n];
        slotLength = new int[n];
        slotObject = new Object3D[n];

        int maxId = 0;
        for (int i = 0; i < n; i++) {
            M3gFile.Obj o = file.objects[i];
            slotType[i] = o.type;
            slotOffset[i] = o.bodyOffset;
            slotLength[i] = o.bodyLength;
            if (o.id > maxId) {
                maxId = o.id;
            }
        }
        byId = new Object3D[maxId + 1];
    }

    static Object3D[] readAll(byte[] data, M3gFile file) throws IOException {
        return readAll(data, file, null, null);
    }

    /**
     * @param base  resource prefix the .m3g was loaded from (e.g. "/res/"),
     *              or null when the caller cannot resolve external images
     * @param log   receives one line per external resource, or null
     */
    static Object3D[] readAll(byte[] data, M3gFile file, String base,
                              StringBuffer log) throws IOException {
        M3gReader r = new M3gReader(data, file);
        r.createShells();
        r.resolveExternalImages(base, log);
        for (int i = 0; i < r.slotObject.length; i++) {
            if (r.slotType[i] != M3gFile.T_EXTERNAL_REF) {
                r.parseObject(i);
            }
        }
        /* Bind pose = the skeleton as stored in the file, before any animate() moves a bone. */
        for (int i = 0; i < r.slotObject.length; i++) {
            if (r.slotObject[i] instanceof SkinnedMesh) {
                ((SkinnedMesh)r.slotObject[i]).captureBindPose();
            }
        }
        /* External-reference slots stay null (their resource is resolved
         * separately) and are not handed back to the application.
         *
         * ORDERING (v04.08) - three passes, and the reason for each:
         *
         *  1. World, if the file has one, goes to index 0.
         *     v03.29 established this for 生化危机: the title does
         *     `loaded[0].find(id)` (BioCtrl.loadSkinnedMesh bci 40..56), so
         *     index 0 must be the scene root. Every Bio .m3g holds its World
         *     near the END of the file (first Node in file order is a plain
         *     Group at index 104 for hunter.m3g), so without this the whole
         *     lookup ran on the wrong object and every characterMesh[] entry
         *     came out null (log134: 39 swallowed NPEs, nodes=0 meshes=0).
         *
         *  2. Every remaining NODE, in file order.
         *  3. Every remaining non-Node (VertexArray/VertexBuffer/IndexBuffer/
         *     Appearance/Material/KeyframeSequence/...), in file order.
         *
         * WHY 2 AND 3 ARE SPLIT - this is the v04.07 fix for 极品飞车:
         * that title addresses objects by POSITION in this array:
         *     ac.b(int id):
         *         g   = n.b_[(id >> 8) & 0xFF]      // logical group -> file
         *         idx = id & 0xFFFF00FF
         *         return (Node) ((Object3D[]) ac.a(g, 2))[idx].duplicate();
         * and every packed id it uses for logical group 6 (which n.b_ maps to
         * the resource pack "/aq") carries idx 0,2,3,6,7,8,9,10,11,12,13,14,
         * 15,16,18,20,21,22,23. In FILE order those positions of /aq are
         * VertexArray / VertexBuffer / PolygonMode / Appearance ..., i.e. not
         * Nodes, so the `checkcast Node` threw ClassCastException, the title's
         * 15 ms TimerTask was cancelled and the loading screen froze
         * (log177: `[TIMEREXC] ... java.lang.ClassCastException: 0` at
         * `ac.b(), bci=32`).
         *
         * MEASURED, NOT ASSUMED: /aq is a pool of ~78 models laid out as
         * [VertexArray x3, VertexBuffer, TriangleStripArray, CompositingMode,
         * PolygonMode, Appearance, Mesh, Group, Group, Group] repeated. With
         * passes 2+3 the array starts Mesh,Group,Group,Group,Mesh,... and the
         * title's indices select exactly "the mesh of model k and some of its
         * bones" - every one of them a Node.
         * Cross-check that this does NOT change 生化危机: all 15 of its .m3g
         * files contain a World, so pass 1 pins index 0 to the same object as
         * before, and BioMain's find(id) does not care about the rest.
         * Also checked: the 3 files whose index 0 must be an Image2D (/bo,
         * /bp, /bs) hold exactly ONE object each and no Nodes, so passes 1-2
         * are no-ops for them.
         *
         * KEEP THIS RULE IN MIND: "index 0 = World when present; Nodes before
         * resources, each group in file order". Changing it will silently
         * re-target every position-addressed title. */
        int total = 0;
        int worldAt = -1;
        for (int i = 0; i < r.slotObject.length; i++) {
            if (r.slotObject[i] != null) {
                total++;
                if (worldAt < 0 && r.slotObject[i] instanceof World) {
                    worldAt = i;
                }
            }
        }
        Object3D[] out = new Object3D[total];
        int k = 0;
        if (worldAt >= 0) {
            out[k++] = r.slotObject[worldAt];
        }
        for (int i = 0; i < r.slotObject.length; i++) {
            Object3D o = r.slotObject[i];
            if (o != null && i != worldAt && o instanceof Node) {
                out[k++] = o;
            }
        }
        for (int i = 0; i < r.slotObject.length; i++) {
            Object3D o = r.slotObject[i];
            if (o != null && i != worldAt && !(o instanceof Node)) {
                out[k++] = o;
            }
        }
        /* v04.26: HOW MANY ROOTS DOES THIS FILE HAVE? It decides the ordering.
         *
         * MEASURED over all 28 .m3g in debug/3d_game (StructChk):
         *   multi-root  af aq aw bl bm i j k l m n o p q y     (13 files)
         *   single-root bz c ca d                              ( 4 files)
         *   no nodes    bo bp bq br bs bt bu bv bw             ( 9 files, 1 obj)
         * and in EVERY single-root file the object at index 0 was NOT the root:
         *   /c  objs[0]=Group(11)  root=Group(0)
         *   /d  objs[0]=Group(12)  root=Group(0)
         *   /bz objs[0]=Mesh(...)  root=Group(0)
         *   /ca objs[0]=Mesh(...)  root=Group(0)
         *
         * WHY THAT MATTERS (log184, 极品飞车):
         *   x.n = new World();                                  // bci=465
         *   x.n.addChild(ac.a(2));      // ac.a(2) == Loader.load("/c")[0]
         *   x.o = x.n.find(10);         // -> NULL, because index 0 is the
         *   x.p = x.n.find(11);         //    11 node and 10 is NOT below it
         *   ab.a(x.o, x.n, out);        // getTransformTo on null -> NPE
         * log183 hit the same shape on "/d": objs[0] was the 12 node while the
         * title also needed 13, which lives in a different branch of the tree.
         * Handing back an interior node makes group 2 (below) unable to help,
         * because an interior node's sub-tree simply does not contain its
         * siblings.
         *
         * THE RULE, chosen so that nothing that works today can move:
         *   - file has NO Node                     -> unchanged (Image2D packs)
         *   - file has exactly ONE top-level node  -> that node goes to index 0,
         *                                             the rest keep file order
         *   - file has SEVERAL top-level nodes     -> COMPLETELY unchanged, the
         *     v04.08 file order is kept because 极品飞车 addresses those tables
         *     POSITIONALLY (`ac.b(raw)`: raw 100..110 = /m /n /p /q /y,
         *     1536..1559 = /af /aq /aw /bl /bm). Re-sorting a multi-root file
         *     is precisely what would bring back the log177 ClassCastException.
         * The middle entry is new and it is the one that fixes log183 AND
         * log184: after it, `loaded[0]` is always the object the title means by
         * "the model", so find() on it (or on a copy, or on a World it was
         * added to) reaches every node of that file through ordinary
         * parent/child links - no out-of-tree shortcuts, which is why
         * getTransformTo() now answers true instead of false. */
        int rootCount = 0;
        int rootAt = -1;
        for (int i = 0; i < out.length; i++) {
            if (out[i] instanceof Node && ((Node)out[i]).parent == null) {
                rootCount++;
                if (rootAt < 0) {
                    rootAt = i;
                }
            }
        }
        if (rootCount == 1 && rootAt > 0) {
            Object3D root = out[rootAt];
            System.arraycopy(out, 0, out, 1, rootAt);
            out[0] = root;
        }
        return out;
    }

    // ------------------------------------------------------------ shells

    private void createShells() throws IOException {
        for (int i = 0; i < slotObject.length; i++) {
            Object3D o;
            switch (slotType[i]) {
            case M3gFile.T_ANIMATION_CONTROLLER:
                o = new AnimationController(true); break;
            case M3gFile.T_ANIMATION_TRACK:
                o = new AnimationTrack(null, 0, null); break;
            case M3gFile.T_APPEARANCE:
                o = new Appearance(); break;
            case M3gFile.T_BACKGROUND:
                o = new Background(); break;
            case M3gFile.T_CAMERA:
                o = new Camera(); break;
            case M3gFile.T_COMPOSITING_MODE:
                o = new CompositingMode(); break;
            case M3gFile.T_FOG:
                o = new Fog(); break;
            case M3gFile.T_POLYGON_MODE:
                o = new PolygonMode(); break;
            case M3gFile.T_GROUP:
                o = new Group(); break;
            case M3gFile.T_IMAGE2D:
                o = new Image2D(); break;
            case M3gFile.T_TRIANGLE_STRIP_ARRAY:
                o = new TriangleStripArray(); break;
            case M3gFile.T_LIGHT:
                o = new Light(); break;
            case M3gFile.T_MATERIAL:
                o = new Material(); break;
            case M3gFile.T_MESH:
                o = new Mesh(true); break;
            case M3gFile.T_MORPHING_MESH:
                o = new MorphingMesh(true); break;
            case M3gFile.T_SKINNED_MESH:
                o = new SkinnedMesh(true); break;
            case M3gFile.T_TEXTURE2D:
                o = new Texture2D(true); break;
            case M3gFile.T_SPRITE3D:
                o = new Sprite3D(true); break;
            case M3gFile.T_KEYFRAME_SEQUENCE:
                o = new KeyframeSequence(); break;
            case M3gFile.T_VERTEX_ARRAY:
                o = new VertexArray(1, 1, 1, new int[1]); break;
            case M3gFile.T_VERTEX_BUFFER:
                o = new VertexBuffer(true); break;
            case M3gFile.T_WORLD:
                o = new World(); break;
            case M3gFile.T_EXTERNAL_REF:
                continue;   /* filled by resolveExternalImages */
            default:
                throw new IOException("unsupported M3G object type "
                                      + slotType[i]);
            }
            slotObject[i] = o;
            M3gFile.Obj meta = file.objects[i];
            if (meta.id > 0 && meta.id < byId.length) {
                byId[meta.id] = o;
            }
        }
    }

    /**
     * References are 1-based indices into the object array offset by two
     * (the reference implementation reserves both 0 and 1); they are NOT
     * userIDs. Verified on all 590 AnimationTrack -> KeyframeSequence
     * references in the test set.
     */
    /**
     * Turns each external-reference slot into an Image2D so texture
     * references resolve. The resource name is a path relative to the .m3g,
     * so it is read through the MIDP image loader (jar-relative when the
     * file came from a MIDlet suite).
     *
     * A missing image is not fatal: the slot becomes an Image2D with no
     * pixels, and Renderer then draws the submesh untectured. That is how a
     * title with partial resources stays playable.
     */
    private void resolveExternalImages(String base, StringBuffer log) {
        for (int i = 0; i < slotObject.length; i++) {
            if (slotType[i] != M3gFile.T_EXTERNAL_REF) {
                continue;
            }
            String name = file.objects[i].externalName;
            Image2D img = null;
            try {
                String path = (base != null) ? join(base, name) : name;
                javax.microedition.lcdui.Image src =
                    javax.microedition.lcdui.Image.createImage(path);
                img = new Image2D(Image2D.RGBA, src);
                if (log != null) {
                    log.append("xref ok ").append(path).append('\n');
                }
            } catch (Throwable t) {
                if (log != null) {
                    log.append("xref miss ").append(name).append(" (")
                       .append(t.toString()).append(")\n");
                }
                /* Record the miss so the renderer/log can report it even
                 * when the caller passes no log buffer. */
                missedImages++;
                img = new Image2D();
                img.format = Image2D.RGBA;
                img.width = 1;
                img.height = 1;
                img.pixels = new int[] { 0xFFFFFFFF };
                img.mutable = false;
                img.textureDirty = true;
            }
            slotObject[i] = img;
        }
    }

    private static String join(String base, String name) {
        if (name.length() > 0 && name.charAt(0) == '/') {
            return name;
        }
        return base.endsWith("/") ? base + name : base + "/" + name;
    }

    private Object3D ref(int value) throws IOException {
        if (value == 0) {
            return null;
        }
        int slot = value - 2;
        if (slot < 0 || slot >= slotObject.length) {
            throw new IOException("M3G reference " + value + " out of range");
        }
        Object3D o = slotObject[slot];
        if (o == null) {
            if (slotType[slot] == M3gFile.T_EXTERNAL_REF) {
                return null;   /* external image, resolved separately */
            }
            throw new IOException("M3G reference " + value + " unresolved");
        }
        return o;
    }

    // ------------------------------------------------------------- parse

    private void parseObject(int slot) throws IOException {
        Cursor c = new Cursor(data, slotOffset[slot],
                              slotOffset[slot] + slotLength[slot]);
        Object3D o = slotObject[slot];
        int type = slotType[slot];

        /* M3gFile already stripped the 4-byte objectID from the body. */
        int userID = file.objects[slot].id;
        o.setUserID(userID);

        int trackCount = c.count();
        int[] trackIDs = new int[trackCount];
        for (int i = 0; i < trackCount; i++) {
            trackIDs[i] = c.u32();
        }
        int paramCount = c.count();
        for (int i = 0; i < paramCount; i++) {
            c.u32();                       /* parameter id */
            c.skip(c.count());             /* parameter bytes */
        }

        switch (type) {
        case M3gFile.T_ANIMATION_CONTROLLER:
            readAnimationController(c, (AnimationController)o);
            break;
        case M3gFile.T_ANIMATION_TRACK:
            readAnimationTrack(c, (AnimationTrack)o);
            break;
        case M3gFile.T_APPEARANCE:
            readAppearance(c, (Appearance)o);
            break;
        case M3gFile.T_BACKGROUND:
            readBackground(c, (Background)o);
            break;
        case M3gFile.T_CAMERA:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readCamera(c, (Camera)o);
            break;
        case M3gFile.T_COMPOSITING_MODE:
            readCompositingMode(c, (CompositingMode)o);
            break;
        case M3gFile.T_FOG:
            readFog(c, (Fog)o);
            break;
        case M3gFile.T_POLYGON_MODE:
            readPolygonMode(c, (PolygonMode)o);
            break;
        case M3gFile.T_GROUP:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readGroupChildren(c, (Group)o);
            break;
        case M3gFile.T_WORLD:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readGroupChildren(c, (Group)o);
            readWorld(c, (World)o);
            break;
        case M3gFile.T_IMAGE2D:
            readImage(c, (Image2D)o);
            break;
        case M3gFile.T_TRIANGLE_STRIP_ARRAY:
            readStrips(c, (TriangleStripArray)o);
            break;
        case M3gFile.T_LIGHT:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readLight(c, (Light)o);
            break;
        case M3gFile.T_MATERIAL:
            readMaterial(c, (Material)o);
            break;
        case M3gFile.T_MESH:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readMeshData(c, (Mesh)o);
            break;
        case M3gFile.T_SKINNED_MESH:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readMeshData(c, (Mesh)o);
            readSkin(c, (SkinnedMesh)o);
            break;
        case M3gFile.T_MORPHING_MESH:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readMeshData(c, (Mesh)o);
            readMorph(c, (MorphingMesh)o);
            break;
        case M3gFile.T_TEXTURE2D:
            readTransformable(c, (Transformable)o);
            readTexture(c, (Texture2D)o);
            break;
        case M3gFile.T_SPRITE3D:
            readTransformable(c, (Transformable)o);
            readNode(c, (Node)o);
            readSprite(c, (Sprite3D)o);
            break;
        case M3gFile.T_KEYFRAME_SEQUENCE:
            readKeyframes(c, (KeyframeSequence)o);
            break;
        case M3gFile.T_VERTEX_ARRAY:
            readVertexArray(c, (VertexArray)o);
            break;
        case M3gFile.T_VERTEX_BUFFER:
            readVertexBuffer(c, (VertexBuffer)o);
            break;
        default:
            throw new IOException("unsupported M3G object type " + type);
        }

        if (c.pos != c.end) {
            throw new IOException(M3gFile.typeName(type) + " #" + userID
                                  + ": consumed " + (c.pos - c.start)
                                  + " of " + (c.end - c.start) + " bytes");
        }

        /* Animation tracks are attached after every object exists. */
        for (int i = 0; i < trackIDs.length; i++) {
            Object3D t = ref(trackIDs[i]);
            if (t instanceof AnimationTrack) {
                o.addAnimationTrack((AnimationTrack)t);
            }
        }
    }

    // ------------------------------------------------------- per type

    private void readTransformable(Cursor c, Transformable t)
            throws IOException {
        if (c.u8() != 0) {
            t.translation[0] = c.f32();
            t.translation[1] = c.f32();
            t.translation[2] = c.f32();
            t.scale[0] = c.f32();
            t.scale[1] = c.f32();
            t.scale[2] = c.f32();
            t.orientation[0] = c.f32();
            t.orientation[1] = c.f32();
            t.orientation[2] = c.f32();
            t.orientation[3] = c.f32();
        }
        if (c.u8() != 0) {
            float[] m = new float[16];
            for (int i = 0; i < 16; i++) {
                m[i] = c.f32();
            }
            Transform tr = new Transform();
            tr.set(m);                     /* M3G stores matrices row-major */
            t.userTransform = tr;
        }
    }

    private void readNode(Cursor c, Node n) throws IOException {
        n.rendering = c.u8() != 0;
        n.picking = (c.u8() != 0) ? 1 : 0;
        n.alpha = (c.u8() & 0xFF) / 255.0f;
        n.scope = c.u32();
        if (c.u8() != 0) {
            c.u8();                        /* z target */
            c.u8();                        /* y target */
            c.u32();                       /* z reference */
            c.u32();                       /* y reference */
        }
    }

    private void readAnimationController(Cursor c, AnimationController a)
            throws IOException {
        float speed = c.f32();
        float weight = c.f32();
        int start = c.u32();
        int end = c.u32();
        c.f32();                           /* reference sequence time */
        c.u32();                           /* reference world time */
        a.loadState(speed, weight, start, end);
    }

    private void readAnimationTrack(Cursor c, AnimationTrack t)
            throws IOException {
        KeyframeSequence seq = (KeyframeSequence)ref(c.u32());
        AnimationController ctl = (AnimationController)ref(c.u32());
        int property = c.u32();
        t.loadState(seq, property, ctl);
    }

    private void readAppearance(Cursor c, Appearance a) throws IOException {
        a.setLayer(c.u8() & 0xFF);
        a.setCompositingMode((CompositingMode)ref(c.u32()));
        a.setFog((Fog)ref(c.u32()));
        a.setPolygonMode((PolygonMode)ref(c.u32()));
        a.setMaterial((Material)ref(c.u32()));
        int n = c.count();
        for (int i = 0; i < n; i++) {
            Texture2D t = (Texture2D)ref(c.u32());
            if (i == 0) {
                a.setTexture(0, t);
            }
        }
    }

    private void readBackground(Cursor c, Background b) throws IOException {
        b.color = c.argb();
        b.image = (Image2D)ref(c.u32());
        b.imageModeX = c.u8();
        b.imageModeY = c.u8();
        b.cropX = c.u32();
        b.cropY = c.u32();
        b.cropW = c.u32();
        b.cropH = c.u32();
        b.colorClear = c.u8() != 0;
        b.depthClear = c.u8() != 0;
    }

    private void readCamera(Cursor c, Camera cam) throws IOException {
        int type = c.u8();
        cam.type = type;
        if (type == Camera.GENERIC) {
            float[] m = new float[16];
            for (int i = 0; i < 16; i++) {
                m[i] = c.f32();
            }
            Transform t = new Transform();
            t.set(m);
            cam.projection.set(t);
        } else {
            cam.fovy = c.f32();
            cam.aspect = c.f32();
            cam.near = c.f32();
            cam.far = c.f32();
        }
    }

    private void readCompositingMode(Cursor c, CompositingMode m)
            throws IOException {
        m.depthTest = c.u8() != 0;
        m.depthWrite = c.u8() != 0;
        m.colorWrite = c.u8() != 0;
        c.u8();                            /* alpha write */
        m.blending = c.u8();
        m.alphaThreshold = (c.u8() & 0xFF) / 255.0f;
        c.f32();                           /* depth offset factor */
        c.f32();                           /* depth offset units */
    }

    private void readFog(Cursor c, Fog f) throws IOException {
        f.color = c.rgb();
        int mode = c.u8();
        f.mode = mode;
        if (mode == Fog.EXPONENTIAL) {
            f.density = c.f32();
        } else {
            f.near = c.f32();
            f.far = c.f32();
        }
    }

    private void readPolygonMode(Cursor c, PolygonMode p) throws IOException {
        p.culling = c.u8();
        p.shading = c.u8();
        p.winding = c.u8();
        p.twoSided = c.u8() != 0;
        p.localCamera = c.u8() != 0;
        c.u8();                            /* perspective correction */
    }

    private void readGroupChildren(Cursor c, Group g) throws IOException {
        int n = c.count();
        for (int i = 0; i < n; i++) {
            Node child = (Node)ref(c.u32());
            if (child != null && child.parent == null) {
                g.addChild(child);
            }
        }
    }

    private void readWorld(Cursor c, World w) throws IOException {
        w.setActiveCamera((Camera)ref(c.u32()));
        w.setBackground((Background)ref(c.u32()));
    }

    private void readLight(Cursor c, Light l) throws IOException {
        l.constant = c.f32();
        l.linear = c.f32();
        l.quadratic = c.f32();
        l.color = c.rgb();
        l.mode = c.u8();
        l.intensity = c.f32();
        c.f32();                           /* spot angle */
        c.f32();                           /* spot exponent */
    }

    private void readMaterial(Cursor c, Material m) throws IOException {
        m.ambient = c.rgb();
        m.diffuse = c.argb() & 0x00FFFFFF;
        m.emissive = c.rgb();
        m.specular = c.rgb();
        m.shininess = c.f32();
        m.vertexColorTracking = c.u8() != 0;
    }

    private void readImage(Cursor c, Image2D img) throws IOException {
        int format = c.u8();
        boolean mutable = c.u8() != 0;
        int w = c.u32();
        int h = c.u32();
        img.format = format;
        img.width = w;
        img.height = h;
        img.mutable = mutable;
        img.pixels = new int[w * h];
        img.textureDirty = true;
        if (mutable) {
            return;                        /* render target: no pixels yet */
        }
        int paletteLength = c.count();
        int[] palette = null;
        if (paletteLength > 0) {
            palette = readPalette(c, paletteLength);
        }
        int pixelLength = c.count();
        int bpp = Image2D.bytesPerPixel(format);
        if (palette != null) {
            decodePaletted(c, img, w, h, palette, pixelLength);
            return;
        }
        if (pixelLength != w * h * bpp) {
            /* v04.05 DIAGNOSTIC: this throw is what breaks 极品飞车 on the
             * device (log176: `[m3g] load FAILED '/bo' (java.io.IOException:
             * Image2D pixel length mismatch)`, then a NullPointerException in
             * the game's per-frame task, then `[TIMEREXC] task ... cancelled`,
             * and the loading screen never advances).
             *
             * The numbers are printed BEFORE throwing so the exact shape of
             * the mismatch is known; the tolerant handling is only added once
             * the real values are on the table. Never guess here: pixelLength
             * smaller than w*h*bpp means the loop below would run off the
             * object body, so "just ignore the check" is not automatically
             * safe. */
            com.sun.m3g.M3gTrace.log("Img2D len " + pixelLength
                + " want " + (w * h * bpp)
                + " format=" + format + " " + w + "x" + h
                + " bpp=" + bpp + " palette=" + paletteLength);
            throw new IOException("Image2D pixel length mismatch");
        }
        for (int i = 0; i < w * h; i++) {
            int a, r, g, b;
            switch (format) {
            case Image2D.ALPHA:
                a = c.u8(); r = g = b = 0xFF; break;
            case Image2D.LUMINANCE:
                r = g = b = c.u8(); a = 0xFF; break;
            case Image2D.LUMINANCE_ALPHA:
                r = g = b = c.u8(); a = c.u8(); break;
            case Image2D.RGB:
                r = c.u8(); g = c.u8(); b = c.u8(); a = 0xFF; break;
            default:
                r = c.u8(); g = c.u8(); b = c.u8(); a = c.u8(); break;
            }
            img.pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    /* ---- v04.05: paletted Image2D --------------------------------------
     *
     * THE BUG THIS FIXES (log176, 极品飞车 stuck on its loading screen):
     * readImage() used to `c.skip(paletteLength)` and then still demand
     * `pixelLength == w*h*bytesPerPixel(format)`. For a paletted image the
     * pixel bytes are INDICES, so the two can never agree. The resulting
     * IOException propagated out of Loader.load(), the title's own code threw
     * a NullPointerException on the model it never got, and TimerThread
     * cancelled that task - so the 15 ms loading animation stopped for good.
     *
     * Measured numbers, reproduced on the host with the shipping reader
     * (debug/3d_game/极品飞车13之变速英文3D版.jar, entry `bo`):
     *     Img2D len 16384 want 49152 format=99(RGB) 128x128 bpp=3 palette=768
     * 768 = 256 entries x 3 bytes -> 8 bits per index -> 128*128*1 = 16384.
     * The file was always correct; the reader was wrong.
     *
     * FORMAT (JSR 184 file format, Image2D):
     *     u32 paletteLength, then that many bytes
     *     u32 pixelLength,   then that many bytes
     * The palette is a list of RGB triples; the number of entries fixes the
     * index width: 2 -> 1 bit, 4 -> 2 bits, 16 -> 4 bits, anything larger ->
     * 8 bits. Rows are padded to a byte boundary. Palette entry 0 is the
     * transparent one, which is why a fully transparent texel comes out as
     * alpha 0 rather than as the colour stored at index 0.
     *
     * Both helpers are deliberately tolerant: a mismatch between the declared
     * pixelLength and the calculated row length is REPORTED but does not
     * abort the model, because one odd image must not take a whole .m3g (and
     * with it the title's main loop) down. Reads stop at the end of
     * pixelLength, so a short body can never run into the next object. */

    /** Reads a paletted image's palette; returns null if the length is unusable. */
    private static int[] readPalette(Cursor c, int paletteLength)
            throws IOException {
        /* RGB triples, but tolerate a 4-byte-per-entry palette: some
         * exporters write RGBA and just call the extra byte padding. Detect
         * by checking which division lands on a power-of-two entry count. */
        int entries = 0;
        int stride = 3;
        int by3 = paletteLength / 3;
        int by4 = paletteLength / 4;
        if (paletteLength % 3 == 0 && (by3 & (by3 - 1)) == 0 && by3 > 0) {
            entries = by3;
        } else if (paletteLength % 4 == 0 && (by4 & (by4 - 1)) == 0 && by4 > 0) {
            entries = by4;
            stride = 4;
        } else if (by3 > 0) {
            entries = by3;               /* not a power of two: take 3 anyway */
        } else {
            c.skip(paletteLength);
            com.sun.m3g.M3gTrace.log("palette " + paletteLength
                + " bytes ignored (unusable)");
            return null;
        }
        int[] palette = new int[entries];
        for (int i = 0; i < entries; i++) {
            int r = c.u8();
            int g = c.u8();
            int b = c.u8();
            int a = 0xFF;
            if (stride == 4) {
                a = c.u8();
            }
            /* index 0 is the transparent one in the M3G paletted encoding */
            palette[i] = (i == 0) ? 0x00000000
                                  : ((a << 24) | (r << 16) | (g << 8) | b);
        }
        return palette;
    }

    /** Expands palette indices into ARGB pixels, stopping at pixelLength. */
    private static void decodePaletted(Cursor c, Image2D img, int w, int h,
                                       int[] palette, int pixelLength)
            throws IOException {
        int entries = palette.length;
        int bits = (entries <= 2) ? 1 : (entries <= 4) ? 2
                 : (entries <= 16) ? 4 : 8;
        int rowBytes = (w * bits + 7) / 8;
        int expect = rowBytes * h;
        int span = pixelLength;          /* never read past the declared body */
        int boxEnd = c.pos + span;

        if (expect != pixelLength) {
            com.sun.m3g.M3gTrace.log("paletted " + w + "x" + h + " bits=" + bits
                + " entries=" + entries + " expected " + expect
                + " bytes, file says " + pixelLength + " (lenient)");
        }

        int total = w * h;
        for (int y = 0; y < h; y++) {
            int rowStart = c.pos;
            for (int x = 0; x < w; x++) {
                int idx;
                if (c.pos >= boxEnd) {
                    /* ran out of declared bytes: leave the rest transparent */
                    return;
                }
                if (bits == 8) {
                    idx = c.u8();
                } else {
                    int byteOff = (x * bits) >> 3;
                    if (rowStart + byteOff >= boxEnd) {
                        return;
                    }
                    int b = c.d[rowStart + byteOff] & 0xFF;
                    int shift = 8 - bits - (((x * bits) & 7));
                    idx = (b >> shift) & ((1 << bits) - 1);
                }
                int i = y * w + x;
                if (i < total) {
                    img.pixels[i] = (idx < entries) ? palette[idx] : 0;
                }
            }
            if (bits == 8) {
                /* consumed w bytes already */
            } else {
                int consumed = rowBytes;
                if (rowStart + consumed > boxEnd) {
                    return;
                }
                c.pos = rowStart + consumed;
            }
        }
        /* Keep the cursor consistent with the declared body length even if
         * the loop converged; the object-length check in parseObject relies
         * on it. */
        if (boxEnd <= c.end) {
            c.pos = boxEnd;
        }
    }

    private void readTexture(Cursor c, Texture2D t) throws IOException {
        t.image = (Image2D)ref(c.u32());
        t.blendColor = c.rgb();
        t.blendMode = c.u8();
        t.wrapS = c.u8();
        t.wrapT = c.u8();
        t.levelFilter = c.u8();
        t.imageFilter = c.u8();
    }

    private void readVertexArray(Cursor c, VertexArray va) throws IOException {
        int componentSize = c.u8();        /* 1 = BYTE, 2 = SHORT */
        if (componentSize != 1 && componentSize != 2) {
            throw new IOException("bad VertexArray component size "
                                  + componentSize);
        }
        int components = c.u8();
        int encoding = c.u8();
        int count = c.u16();
        int[] values = new int[count * components];
        int[] previous = new int[4];

        if (componentSize == 1) {
            for (int i = 0; i < count; i++) {
                for (int j = 0; j < components; j++) {
                    int raw = c.u8();
                    if (encoding == 0) {
                        previous[j] = (byte)raw;
                    } else {
                        previous[j] = (byte)(previous[j] + (byte)raw);
                    }
                    values[i * components + j] = previous[j];
                }
            }
        } else {
            for (int i = 0; i < count; i++) {
                for (int j = 0; j < components; j++) {
                    int raw = c.u16();
                    if (encoding == 0) {
                        previous[j] = (short)raw;
                    } else {
                        previous[j] = (short)(previous[j] + (short)raw);
                    }
                    values[i * components + j] = previous[j];
                }
            }
        }

        va.numVertices = count;
        va.numComponents = components;
        va.componentSize = componentSize;
        va.data = values;
    }

    private void readVertexBuffer(Cursor c, VertexBuffer vb)
            throws IOException {
        vb.defaultColor = new float[4];
        int argb = c.argb();
        vb.defaultColor[0] = ((argb >> 16) & 0xFF) / 255.0f;
        vb.defaultColor[1] = ((argb >> 8) & 0xFF) / 255.0f;
        vb.defaultColor[2] = (argb & 0xFF) / 255.0f;
        vb.defaultColor[3] = ((argb >> 24) & 0xFF) / 255.0f;

        vb.positions = (VertexArray)ref(c.u32());
        vb.positionBias[0] = c.f32();
        vb.positionBias[1] = c.f32();
        vb.positionBias[2] = c.f32();
        vb.positionScale = c.f32();

        vb.normals = (VertexArray)ref(c.u32());
        vb.colors = (VertexArray)ref(c.u32());

        int texUnits = c.count();
        for (int i = 0; i < texUnits; i++) {
            VertexArray ta = (VertexArray)ref(c.u32());
            float b0 = c.f32();
            float b1 = c.f32();
            float b2 = c.f32();
            float s = c.f32();
            if (i == 0) {
                vb.texCoords = ta;
                vb.texBias[0] = b0;
                vb.texBias[1] = b1;
                vb.texBias[2] = b2;
                vb.texScale = s;
            }
        }
        if (vb.positions != null) {
            vb.numVertices = vb.positions.numVertices;
        }
    }

    private void readStrips(Cursor c, TriangleStripArray tsa)
            throws IOException {
        int encoding = c.u8();
        int[] indices;
        int startIndex = 0;
        int indexCount = 0;

        if (encoding == 0) {
            startIndex = c.u32();
        } else if (encoding == 1) {
            startIndex = c.u8();
        } else if (encoding == 2) {
            startIndex = c.u16();
        } else if (encoding >= 128 && encoding <= 130) {
            indexCount = c.count();
            indices = new int[indexCount];
            for (int i = 0; i < indexCount; i++) {
                indices[i] = (encoding == 128) ? c.u32()
                           : (encoding == 129) ? c.u8() : c.u16();
            }
            int lengthCount = c.count();
            tsa.indices = indices;
            tsa.strips = new int[lengthCount];
            for (int i = 0; i < lengthCount; i++) {
                tsa.strips[i] = c.u32();
            }
            return;
        } else {
            throw new IOException("bad strip encoding " + encoding);
        }

        int lengthCount = c.count();
        int[] lengths = new int[lengthCount];
        int total = 0;
        for (int i = 0; i < lengthCount; i++) {
            lengths[i] = c.u32();
            if (lengths[i] < 3) {
                throw new IOException("strip shorter than 3");
            }
            total += lengths[i];
        }
        indices = new int[total];
        for (int i = 0; i < total; i++) {
            indices[i] = startIndex + i;
        }
        tsa.indices = indices;
        tsa.strips = lengths;
    }

    private void readKeyframes(Cursor c, KeyframeSequence k)
            throws IOException {
        int interpolation = c.u8();
        int repeatMode = c.u8();
        int encoding = c.u8();
        int duration = c.u32();
        int rangeFirst = c.u32();
        int rangeLast = c.u32();
        int components = c.u32();
        int keyFrames = c.count();

        k.interpolation = interpolation;
        k.repeatMode = repeatMode;
        k.duration = duration;
        k.validRangeStart = rangeFirst;
        k.validRangeEnd = rangeLast;
        k.keyframeCount = keyFrames;
        k.componentCount = components;
        k.times = new int[keyFrames];
        k.values = new float[keyFrames * components];

        if (encoding == 0) {
            for (int i = 0; i < keyFrames; i++) {
                k.times[i] = c.u32();
                for (int j = 0; j < components; j++) {
                    k.values[i * components + j] = c.f32();
                }
            }
            return;
        }

        float[] bias = new float[components];
        float[] scale = new float[components];
        for (int j = 0; j < components; j++) {
            bias[j] = c.f32();
        }
        for (int j = 0; j < components; j++) {
            scale[j] = c.f32();
        }
        for (int i = 0; i < keyFrames; i++) {
            k.times[i] = c.u32();
            for (int j = 0; j < components; j++) {
                int raw = (encoding == 1) ? c.u8() : c.u16();
                float norm = (encoding == 1) ? 255.0f : 65535.0f;
                k.values[i * components + j] = bias[j]
                                             + scale[j] * raw / norm;
            }
        }
    }

    private void readMeshData(Cursor c, Mesh m) throws IOException {
        m.vertexBuffer = (VertexBuffer)ref(c.u32());
        int subMeshes = c.count();
        m.appearances = new Appearance[subMeshes];
        m.indexBuffers = new IndexBuffer[subMeshes];
        for (int i = 0; i < subMeshes; i++) {
            m.indexBuffers[i] = (IndexBuffer)ref(c.u32());
            m.appearances[i] = (Appearance)ref(c.u32());
        }
        m.indexBuffer = (subMeshes > 0) ? m.indexBuffers[0] : null;
    }

    private void readSkin(Cursor c, SkinnedMesh s) throws IOException {
        s.skeleton = (Group)ref(c.u32());
        int n = c.count();
        s.bones = new Node[n];
        s.firstVertex = new int[n];
        s.vertexCount = new int[n];
        s.boneWeights = new int[n];
        for (int i = 0; i < n; i++) {
            s.bones[i] = (Node)ref(c.u32());
            s.firstVertex[i] = c.u32();
            s.vertexCount[i] = c.u32();
            s.boneWeights[i] = c.u32();
        }
    }

    private void readMorph(Cursor c, MorphingMesh m) throws IOException {
        int n = c.count();
        m.targets = new VertexBuffer[n];
        m.weights = new float[n];
        for (int i = 0; i < n; i++) {
            m.targets[i] = (VertexBuffer)ref(c.u32());
            m.weights[i] = c.f32();
        }
    }

    private void readSprite(Cursor c, Sprite3D s) throws IOException {
        s.image = (Image2D)ref(c.u32());
        s.appearance = (Appearance)ref(c.u32());
        s.scaled = c.u8() != 0;
        s.cropX = c.u32();
        s.cropY = c.u32();
        s.cropW = c.u32();
        s.cropH = c.u32();
    }

    // ------------------------------------------------------------ cursor

    /** Little-endian reader over one object body. */
    private static final class Cursor {
        final byte[] d;
        final int start;
        final int end;
        int pos;

        Cursor(byte[] data, int from, int to) {
            d = data;
            start = from;
            pos = from;
            end = to;
        }

        private void need(int n) throws IOException {
            if (n < 0 || end - pos < n) {
                throw new IOException("truncated M3G object at " + pos);
            }
        }

        int u8() throws IOException {
            need(1);
            return d[pos++] & 0xFF;
        }

        int u16() throws IOException {
            need(2);
            int v = (d[pos] & 0xFF) | ((d[pos + 1] & 0xFF) << 8);
            pos += 2;
            return v;
        }

        int u32() throws IOException {
            need(4);
            int v = (d[pos] & 0xFF) | ((d[pos + 1] & 0xFF) << 8)
                  | ((d[pos + 2] & 0xFF) << 16) | ((d[pos + 3] & 0xFF) << 24);
            pos += 4;
            return v;
        }

        float f32() throws IOException {
            return Float.intBitsToFloat(u32());
        }

        /** Count field: rejected when it cannot possibly fit (overflow). */
        int count() throws IOException {
            int n = u32();
            if (n < 0 || n > end - pos) {
                throw new IOException("implausible count " + n + " at " + pos);
            }
            return n;
        }

        void skip(int n) throws IOException {
            need(n);
            pos += n;
        }

        /** 3 bytes: R, G, B. */
        int rgb() throws IOException {
            int r = u8();
            int g = u8();
            int b = u8();
            return (r << 16) | (g << 8) | b;
        }

        /** 4 bytes stored B, G, R, A. */
        int argb() throws IOException {
            int b = u8();
            int g = u8();
            int r = u8();
            int a = u8();
            return (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
}
