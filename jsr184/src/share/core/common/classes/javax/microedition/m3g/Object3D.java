package javax.microedition.m3g;

import java.util.Vector;

/**
 * Base of every M3G object: user ID plus the animation track list.
 */
public abstract class Object3D {

    int userID = 0;
    private Vector tracks; /* AnimationTrack */

    Object3D() {
    }

    public int getUserID() {
        return userID;
    }

    public void setUserID(int id) {
        userID = id;
    }

    public Object3D find(int userID) {
        return findInternal(userID);
    }

    /**
     * Looks up an object by user ID, searching THIS object and everything it
     * references (children, vertex/index buffers, images, appearances, ...).
     * <p>
     * v03.29: this used to test only `this.userID`, so
     * `loaded[0].find(id)` returned null for every id except the root's.
     * log134 showed the consequence: the CAPCOM title does
     *     characterModels[i] = (SkinnedMesh) loaded[0].find(meshItemIndex[i][0]);
     * (BioCtrl.loadSkinnedMesh bci 40..56), so every entry became null and the
     * frame called g3d.render(null, transform), which threw NullPointerException
     * from renderNode() - 39 times per session, all swallowed by the title's own
     * catch. Nothing was ever drawn: log134 has nodes=0 meshes=0 on every frame.
     * JSR-184 defines find() as a search of the whole object graph, which is
     * what collectReferences() already describes, so the traversal reuses it.
     */
    Object3D findInternal(int id) {
        if (id == 0) {
            return null;
        }
        if (userID != 0 && userID == id) {
            return this;
        }
        /* Breadth-first over collectReferences(). Visited markers are not
         * needed: the reference graph is a DAG in practice and each object is
         * enqueued at most once per edge, which the size cap below bounds. */
        Vector queue = new Vector();
        Vector seen = new Vector();
        queue.addElement(this);
        seen.addElement(this);
        for (int head = 0; head < queue.size() && head < 8192; head++) {
            Object3D cur = (Object3D)queue.elementAt(head);
            Vector refs = new Vector();
            cur.collectReferences(refs);
            for (int i = 0; i < refs.size(); i++) {
                Object3D r = (Object3D)refs.elementAt(i);
                if (r == null || seen.contains(r)) {
                    continue;
                }
                seen.addElement(r);
                if (r.userID != 0 && r.userID == id) {
                    return r;
                }
                queue.addElement(r);
            }
        }
        return null;
    }

    public void addAnimationTrack(AnimationTrack t) {
        if (t == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (t.getTarget() != null) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("track already bound"));
        }
        if (tracks == null) {
            tracks = new Vector();
        }
        tracks.addElement(t);
        t.setTarget(this);
    }

    public AnimationTrack getAnimationTrack(int index) {
        if (tracks == null || index < 0 || index >= tracks.size()) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return (AnimationTrack)tracks.elementAt(index);
    }

    public void removeAnimationTrack(AnimationTrack t) {
        if (tracks != null && t != null) {
            tracks.removeElement(t);
            t.setTarget(null);
        }
    }

    public int getAnimationTrackCount() {
        return tracks == null ? 0 : tracks.size();
    }

    /* ---- M3G 1.1 user parameters ---- */

    private java.util.Hashtable userObject = null;

    public void setUserObject(Object userObject) {
        if (userObject instanceof java.util.Hashtable) {
            this.userObject = (java.util.Hashtable)userObject;
        } else {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("user object must be a Hashtable"));
        }
    }

    public Object getUserObject() {
        return userObject;
    }

    /**
     * Enumerates the Object3D instances this one references.
     *
     * M3G 1.1 API; titles walk the graph with it (the CAPCOM title uses it
     * to retarget textures). With refs == null it returns the count, which
     * is what the two-call idiom below relies on:
     *     int n = root.getReferences(null);
     *     Object3D[] a = new Object3D[n];
     *     root.getReferences(a);
     */
    public int getReferences(Object3D[] refs) {
        java.util.Vector v = new java.util.Vector();
        collectReferences(v);
        if (refs != null) {
            int n = (refs.length < v.size()) ? refs.length : v.size();
            for (int i = 0; i < n; i++) {
                refs[i] = (Object3D)v.elementAt(i);
            }
        }
        return v.size();
    }

    /** Subclasses append their outgoing references; order is not defined. */
    void collectReferences(java.util.Vector out) {
        /* v03.29: animation tracks are part of the object graph as far as
         * find() is concerned; without them the KeyframeSequences in a model are
         * unreachable from the root.
         */
        if (tracks != null) {
            for (int i = 0; i < tracks.size(); i++) {
                out.addElement(tracks.elementAt(i));
            }
        }
    }

    private int animStamp = 0;
    private static int animStampCounter = 0;
    private static int dbgAnim = 0;

    /**
     * Samples the animation tracks of this object AND of every object reachable from it (a SkinnedMesh's skeleton
     * carries all the tracks of its model, the mesh itself none), at the given world time. Returns
     * Integer.MAX_VALUE: the time to the next keyframe is not computed. M3G 1.1 API.
     * v03.57: was the object's own tracks only. A stamp marks visited objects so the walk stays linear.
     */
    public int animate(int worldTime) {
        int stamp = ++animStampCounter;
        Vector queue = new Vector();
        Vector refs = new Vector();
        queue.addElement(this);
        animStamp = stamp;
        int tracks = 0;
        for (int head = 0; head < queue.size() && head < 8192; head++) {
            Object3D cur = (Object3D)queue.elementAt(head);
            tracks += cur.getAnimationTrackCount();
            cur.animateSelf(worldTime);
            refs.removeAllElements();
            cur.collectReferences(refs);
            for (int i = 0; i < refs.size(); i++) {
                Object3D r = (Object3D)refs.elementAt(i);
                if (r == null || r.animStamp == stamp) {
                    continue;
                }
                r.animStamp = stamp;
                queue.addElement(r);
            }
        }
        if (dbgAnim < 3) {
            dbgAnim++;
            com.sun.m3g.M3gTrace.log("animate t=" + worldTime + " root=" + getClass().getName()
                + " reached=" + queue.size() + " tracks=" + tracks);
        }
        return Integer.MAX_VALUE;
    }

    /** This object's own tracks only (what Renderer.walk applies per node while it traverses). */
    int animateSelf(int worldTime) {
        int valid = Integer.MAX_VALUE;
        int n = getAnimationTrackCount();
        for (int i = 0; i < n; i++) {
            AnimationTrack t = getAnimationTrack(i);
            if (t.controller == null || t.sequence == null) {
                continue;
            }
            t.controller.advanceTo(worldTime);
            float[] v = t.sample();
            if (v == null) {
                continue;
            }
            applyAnimation(t.property, v);
        }
        return valid;
    }

    /**
     * Deep-copies this object (M3G 1.1). Titles use it to place several
     * instances of a loaded model.
     *
     * CLDC has no Object.clone(), so each type is copied through its own
     * constructor. Immutable data (vertex arrays, index buffers, images) is
     * shared by reference, which matches the spec: only the object itself
     * has to be distinct.
     *
     * v04.24: the user ID is carried over in ONE place here rather than in each
     * branch, because forgetting it in a branch is exactly the bug log183 hit:
     * 极品飞车 takes `Node model = ac.a(3)` (== duplicate() of the Group with
     * userID 12 in '/d') and then does `(Group) model.find(12)`. With the ID
     * reset to 0, find() searched for a *child* with ID 12 that never existed,
     * returned null, and the title's next call, addChild on that null, threw
     * NullPointerException (log183: ab.a() bci=135 <- x.q() bci=196).
     * The title confirms the intent itself: ab.b(Node) explicitly does
     * duplicate() followed by setUserID(getUserID()), i.e. a copy is expected
     * to carry the ID that find() looks up.
     */
    public Object3D duplicate() {
        Object3D d = duplicateInternal();
        if (d != null && d != this) {
            d.userID = this.userID;
            /* NOTE: no scene-graph shortcut is copied here on purpose. find()
             * must only ever answer with an object that is reachable through
             * the copy's OWN parent/child links, otherwise getTransformTo()
             * reports "not in the same scene graph" and the title silently
             * gets a wrong matrix. The log184 NPE was fixed by making
             * Loader.load() hand back the real root (M3gReader.readAll), not by
             * letting find() escape the tree. */
        }
        return d;
    }

    private Object3D duplicateInternal() {
        if (this instanceof Mesh) {
            return ((Mesh)this).duplicateMesh();
        }
        if (this instanceof Sprite3D) {
            Sprite3D s = (Sprite3D)this;
            Sprite3D c = new Sprite3D(s.isScaled(), s.getImage(),
                                      s.getAppearance());
            c.setCrop(s.getCropX(), s.getCropY(), s.getCropWidth(),
                      s.getCropHeight());
            copyNodeState(s, c);
            return c;
        }
        if (this instanceof Camera) {
            Camera c = new Camera();
            copyTransformableState((Camera)this, c);
            copyNodeState((Camera)this, c);
            return c;
        }
        if (this instanceof Light) {
            Light c = new Light();
            c.setMode(((Light)this).getMode());
            c.setColor(((Light)this).getColor());
            c.setIntensity(((Light)this).getIntensity());
            copyNodeState((Light)this, c);
            return c;
        }
        if (this instanceof Group) {
            Group src = (Group)this;
            Group c = new Group();
            copyNodeState(src, c);
            /* v04.24: the children must come along. Before this the branch
             * returned a CHILDLESS Group, so duplicating a loaded model gave an
             * empty shell: everything drawn below that node disappeared, and
             * find() on the copy could not see anything the original held.
             * log183 is the second half of that: 极品飞车's '/d' is ONE tree
             * whose nodes carry userID 12 and 13, the copy of the 12 node was
             * childless, so find(13) never had a chance and the next addChild
             * threw NullPointerException (ab.a() bci=855).
             * Recursion is safe here: addChild() rejects a node that already
             * has a parent, and an M3G file is a tree, so each node is reached
             * once. Vertex data is NOT copied - Mesh.duplicateMesh() above
             * shares the immutable buffers, which is what the spec asks for. */
            for (int i = 0; i < src.childCountInternal(); i++) {
                c.addChild((Node)src.childAtInternal(i).duplicate());
            }
            return c;
        }
        if (this instanceof Appearance) {
            Appearance a = (Appearance)this;
            Appearance c = new Appearance();
            c.setMaterial(a.getMaterial());
            c.setPolygonMode(a.getPolygonMode());
            c.setCompositingMode(a.getCompositingMode());
            c.setFog(a.getFog());
            if (a.getTextureCount() > 0) {
                c.setTexture(0, a.getTexture(0));
            }
            return c;
        }
        /* Types nobody duplicates in practice keep the same instance; a
         * wrong deep copy would be worse than a shared immutable one. */
        return this;
    }

    static void copyTransformableState(Transformable src, Transformable dst) {
        System.arraycopy(src.translation, 0, dst.translation, 0, 3);
        System.arraycopy(src.scale, 0, dst.scale, 0, 3);
        System.arraycopy(src.orientation, 0, dst.orientation, 0, 4);
        dst.userTransform = src.userTransform;
    }

    static void copyNodeState(Node src, Node dst) {
        if (src instanceof Transformable && dst instanceof Transformable) {
            copyTransformableState((Transformable)src, (Transformable)dst);
        }
        dst.alpha = src.alpha;
        dst.scope = src.scope;
        dst.picking = src.picking;
        dst.rendering = src.rendering;
        /* userID is NOT set here: duplicate() assigns it once for every branch,
         * which is the only way to be sure no branch can forget it. See the
         * comment on duplicate() for the log183 failure this prevents. */
    }

    /** Number of properties an AnimationTrack may animate on this class. */
    int getPropertyCount() {
        return 0;
    }

    /** Applied by Renderer before a frame when animation is enabled. */
    void applyAnimation(int property, float[] value) {
    }
}
