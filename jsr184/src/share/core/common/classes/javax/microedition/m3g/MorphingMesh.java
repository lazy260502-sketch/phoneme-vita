package javax.microedition.m3g;

/**
 * Mesh whose vertex positions are blended from several VertexBuffers.
 * The targets and weights are loaded and stored here; the blending itself is
 * done in Renderer.applyMorph() (v04.43):
 *     v = base + sum_i w_i * (target_i - base)
 */
public class MorphingMesh extends Mesh {

    VertexBuffer[] targets = null;
    float[] weights = null;

    MorphingMesh(boolean placeholder) {
        super(true);
    }

    public MorphingMesh(VertexBuffer base, VertexBuffer[] targets,
                        IndexBuffer indexBuffer, Appearance appearance) {
        super(base, indexBuffer, appearance);
        if (targets == null || targets.length == 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("no morph targets"));
        }
        this.targets = new VertexBuffer[targets.length];
        System.arraycopy(targets, 0, this.targets, 0, targets.length);
        this.weights = new float[targets.length];
    }

    void collectReferences(java.util.Vector out) {
        if (targets != null) {
            for (int i = 0; i < targets.length; i++) {
                if (targets[i] != null) out.addElement(targets[i]);
            }
        }
        super.collectReferences(out);
    }

    public int getMorphTargetCount() {
        return targets == null ? 0 : targets.length;
    }

    public VertexBuffer getMorphTarget(int index) {
        if (targets == null || index < 0 || index >= targets.length) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        return targets[index];
    }

    public void setWeights(float[] w) {
        if (w == null || weights == null || w.length < weights.length) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad weights"));
        }
        System.arraycopy(w, 0, weights, 0, weights.length);
    }

    public void getWeights(float[] w) {
        if (w == null || weights == null || w.length < weights.length) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad weights"));
        }
        System.arraycopy(weights, 0, w, 0, weights.length);
    }
}
