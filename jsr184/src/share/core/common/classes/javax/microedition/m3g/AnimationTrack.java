package javax.microedition.m3g;

/**
 * Binds a KeyframeSequence to one animatable property of an Object3D.
 */
public class AnimationTrack extends Object3D {

    /* Target property codes (JSR 184). */
    public static final int ALPHA = 256;
    public static final int AMBIENT_COLOR = 257;
    public static final int COLOR = 258;
    public static final int CROP = 259;
    public static final int DENSITY = 260;
    public static final int DIFFUSE_COLOR = 261;
    public static final int EMISSIVE_COLOR = 262;
    public static final int FAR_DISTANCE = 263;
    public static final int FIELD_OF_VIEW = 264;
    public static final int INTENSITY = 265;
    public static final int MORPH_WEIGHTS = 266;
    public static final int NEAR_DISTANCE = 267;
    public static final int ORIENTATION = 268;
    public static final int PICKABILITY = 269;
    public static final int SCALE = 270;
    public static final int SHININESS = 271;
    public static final int SPECULAR_COLOR = 272;
    public static final int SPOT_ANGLE = 273;
    public static final int SPOT_EXPONENT = 274;
    public static final int TRANSLATION = 275;
    public static final int VISIBILITY = 276;

    KeyframeSequence sequence;    int property;
    AnimationController controller;
    private Object3D target = null;

    AnimationTrack(KeyframeSequence sequence, int property,
                  AnimationController controller) {
        this.sequence = sequence;
        this.property = property;
        this.controller = controller != null ? controller
                                            : new AnimationController();
    }

    public AnimationTrack(KeyframeSequence sequence, int property) {
        if (sequence == null) {
            throw com.sun.m3g.M3gTrace.fail(new NullPointerException());
        }
        this.sequence = sequence;
        this.property = property;
        this.controller = new AnimationController();
    }

    /** Loader path. */
    void loadState(KeyframeSequence sequence, int property,
                   AnimationController controller) {
        this.sequence = sequence;
        this.property = property;
        this.controller = controller != null ? controller
                                            : new AnimationController();
    }

    public KeyframeSequence getKeyframeSequence() {
        return sequence;
    }

    public int getTargetProperty() {
        return property;
    }

    public AnimationController getController() {
        return controller;
    }

    void setTarget(Object3D t) {
        target = t;
    }

    Object3D getTarget() {
        return target;
    }

    /**
     * Sampled value at the controller position, or null when disabled.
     * v03.57: LOOP wraps by the sequence duration (was clamped), STEP holds, SLERP/SQUAD interpolate quaternions
     * (SPLINE and SQUAD fall back to LINEAR and SLERP: no model here uses them), binary search over the valid range.
     */
    float[] sample() {
        if (!controller.isActive() || sequence == null) {
            return null;
        }
        KeyframeSequence s = sequence;
        int n = s.keyframeCount;
        int c = s.componentCount;
        if (n <= 0 || c <= 0 || s.times == null || s.values == null) {
            return null;
        }
        int first = s.validRangeStart;
        int last = s.validRangeEnd;
        if (first < 0 || last >= n || last < first) {
            first = 0;
            last = n - 1;
        }
        int dur = s.duration;
        boolean loop = s.repeatMode == KeyframeSequence.LOOP && dur > 0;
        int t = controller.getPosition(0);
        if (loop) {
            t = t % dur;
        }
        int ft = s.times[first];
        int lt = s.times[last];
        int i;
        int j;
        int ti;
        int tj;
        if (first == last) {
            return copyKey(s, first);
        }
        if (t < ft) {
            if (!loop) {
                return copyKey(s, first);
            }
            i = last;
            j = first;
            ti = lt - dur;
            tj = ft;
        } else if (t >= lt) {
            if (!loop || ft + dur <= lt) {
                return copyKey(s, last);
            }
            i = last;
            j = first;
            ti = lt;
            tj = ft + dur;
        } else {
            int lo = first;
            int hi = last;
            while (hi - lo > 1) {
                int mid = (lo + hi) >> 1;
                if (s.times[mid] <= t) {
                    lo = mid;
                } else {
                    hi = mid;
                }
            }
            i = lo;
            j = lo + 1;
            ti = s.times[i];
            tj = s.times[j];
        }
        if (s.interpolation == KeyframeSequence.STEP) {
            return copyKey(s, i);
        }
        float f = (tj > ti) ? (float)(t - ti) / (float)(tj - ti) : 0.0f;
        float[] v = new float[c];
        if (c == 4 && (s.interpolation == KeyframeSequence.SLERP
                       || s.interpolation == KeyframeSequence.SQUAD)) {
            slerp(v, s.values, i * c, j * c, f);
            return v;
        }
        for (int k = 0; k < c; k++) {
            float a = s.values[i * c + k];
            float b = s.values[j * c + k];
            v[k] = a + (b - a) * f;
        }
        return v;
    }

    private static float[] copyKey(KeyframeSequence s, int index) {
        float[] v = new float[s.componentCount];
        System.arraycopy(s.values, index * s.componentCount, v, 0, s.componentCount);
        return v;
    }

    /** Spherical interpolation of the unit quaternions at a and b (x, y, z, w), shortest arc, result normalised. */
    static void slerp(float[] out, float[] q, int a, int b, float f) {
        float ax = q[a], ay = q[a + 1], az = q[a + 2], aw = q[a + 3];
        float bx = q[b], by = q[b + 1], bz = q[b + 2], bw = q[b + 3];
        float dot = ax * bx + ay * by + az * bz + aw * bw;
        if (dot < 0.0f) {
            bx = -bx;
            by = -by;
            bz = -bz;
            bw = -bw;
            dot = -dot;
        }
        float k0;
        float k1;
        if (dot > 0.9995f) {
            k0 = 1.0f - f;
            k1 = f;
        } else {
            double sn = Math.sqrt(1.0 - (double)dot * (double)dot);
            double th = com.sun.m3g.Matrix4.atan2(sn, (double)dot);
            k0 = (float)(Math.sin((1.0 - (double)f) * th) / sn);
            k1 = (float)(Math.sin((double)f * th) / sn);
        }
        float x = k0 * ax + k1 * bx;
        float y = k0 * ay + k1 * by;
        float z = k0 * az + k1 * bz;
        float w = k0 * aw + k1 * bw;
        float len = (float)Math.sqrt(x * x + y * y + z * z + w * w);
        if (len > 0.0f) {
            x /= len;
            y /= len;
            z /= len;
            w /= len;
        }
        out[0] = x;
        out[1] = y;
        out[2] = z;
        out[3] = w;
    }

    void collectReferences(java.util.Vector out) {
        if (sequence != null) {
            out.addElement(sequence);
        }
        super.collectReferences(out);
    }
}
