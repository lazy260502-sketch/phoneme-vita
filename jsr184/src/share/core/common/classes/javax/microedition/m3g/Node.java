package javax.microedition.m3g;

import com.sun.m3g.Matrix4;

import java.util.Vector;

/**
 * Scene graph node: transform plus the rendering flags every drawable
 * inherits.
 */
public abstract class Node extends Transformable {

    public static final int NONE = -1;
    public static final int ORIGIN = -2;
    public static final int X_AXIS = 0;
    public static final int Y_AXIS = 1;
    public static final int Z_AXIS = 2;

    float alpha = 1.0f;
    int scope = -1;
    int picking = 0;
    boolean rendering = true;
    Group parent = null;

    Node() {
    }

    public float getAlpha() {
        return alpha;
    }

    public void setAlpha(float a) {
        if (a < 0.0f || a > 1.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("alpha out of range"));
        }
        alpha = a;
    }

    public int getScope() {
        return scope;
    }

    public void setScope(int s) {
        scope = s;
    }

    public int getPicking() {
        return picking;
    }

    public void setPicking(int p) {
        picking = p;
    }

    /** World-space composite matrix (used by Renderer and alignment). */
    void getWorldMatrix(float[] out) {
        float[] local = new float[16];
        getCompositeMatrix(local);
        if (parent == null) {
            System.arraycopy(local, 0, out, 0, 16);
        } else {
            float[] pm = new float[16];
            parent.getWorldMatrix(pm);
            Matrix4.mul(out, pm, local);
        }
    }

    /** World position of the node origin. */
    void getWorldPosition(float[] out3) {
        float[] m = new float[16];
        getWorldMatrix(m);
        out3[0] = m[12];
        out3[1] = m[13];
        out3[2] = m[14];
    }

    /* v03.30: JSR-184 declares `Node getParent()`. This returned `Group`, so
     * a title compiled against the real API (NFS: invokevirtual
     * Node.getParent:()Ljavax/microedition/m3g/Node;) hit NoSuchMethodError
     * - the JVM resolves by the full descriptor, the return type included. */
    public Node getParent() {
        return parent;
    }

    public void setRenderingEnable(boolean enable) {
        rendering = enable;
    }

    public boolean isRenderingEnabled() {
        return rendering;
    }

    public void setAlphaFactor(float alpha) {
        if (alpha < 0.0f || alpha > 1.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("alpha out of range"));
        }
        this.alpha = alpha;
    }

    public float getAlphaFactor() {
        return alpha;
    }

    /**
     * Writes this node's transform expressed in another node's coordinate
     * system. Titles use it to attach one model to a bone of another.
     */
    /* v03.30: JSR-184 declares `boolean getTransformTo(Node, Transform)`;
     * this was `void` (NFS: invokevirtual ...getTransformTo:(Node;Transform;)Z
     * -> NoSuchMethodError). The result is false when the two nodes are not
     * in the same scene graph (no common root), in which case the transform
     * is left untouched. */
    public boolean getTransformTo(Node target, Transform transform) {
        if (transform == null || target == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        if (rootOf(this) != rootOf(target)) {
            return false;
        }
        float[] mine = new float[16];
        getWorldMatrix(mine);
        float[] theirs = new float[16];
        target.getWorldMatrix(theirs);
        float[] inv = new float[16];
        if (!Matrix4.invert(inv, theirs)) {
            /* Singular target: the spec reports "no transform" as false. */
            return false;
        }
        float[] result = new float[16];
        Matrix4.mul(result, inv, mine);
        System.arraycopy(result, 0, transform.m, 0, 16);
        return true;
    }

    private static Node rootOf(Node n) {
        while (n.parent != null) {
            n = n.parent;
        }
        return n;
    }

    /** Applies an animated property to this node. */
    void applyAnimation(int property, float[] v) {
        switch (property) {
        case AnimationTrack.TRANSLATION:
            if (v.length >= 3) {
                translation[0] = v[0];
                translation[1] = v[1];
                translation[2] = v[2];
            }
            break;
        case AnimationTrack.SCALE:
            if (v.length >= 3) {
                scale[0] = v[0];
                scale[1] = v[1];
                scale[2] = v[2];
            } else if (v.length == 1) {
                scale[0] = scale[1] = scale[2] = v[0];
            }
            break;
        case AnimationTrack.ORIENTATION:
            if (v.length >= 4) {
                setOrientationQuat(v[0], v[1], v[2], v[3]);
            }
            break;
        case AnimationTrack.ALPHA:
            if (v.length >= 1) {
                alpha = v[0];
            }
            break;
        case AnimationTrack.PICKABILITY:
            if (v.length >= 1) {
                picking = (int)v[0];
            }
            break;
        case AnimationTrack.VISIBILITY:
            if (v.length >= 1) {
                rendering = v[0] > 0.5f;
            }
            break;
        default:
            break;
        }
    }

    Vector animationTracksOf() {
        return null;
    }
}
