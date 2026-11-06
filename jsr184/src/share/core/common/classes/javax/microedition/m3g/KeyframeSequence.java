package javax.microedition.m3g;

/**
 * Animation data holders. Tracks are stored on their target Object3D and
 * sampled by AnimationTrack.sample().
 */
public class KeyframeSequence extends Object3D {

    /* v03.57: JSR-184 values. The file stores them as raw bytes (jill_han.m3g: 176 on vector tracks, 177 on all
     * quaternion tracks, 180 on one two-key scale track, repeat mode 193 everywhere); LINEAR was 193 here. */
    public static final int LINEAR = 176;
    public static final int SLERP = 177;
    public static final int SPLINE = 178;
    public static final int SQUAD = 179;
    public static final int STEP = 180;
    public static final int CONSTANT = 192;
    public static final int LOOP = 193;

    public static final int WRAP = 240;
    public static final int CLAMP = 241;

    int interpolation = LINEAR;
    int repeatMode = CONSTANT;
    int duration = 0;
    int validRangeStart = 0;
    int validRangeEnd = 0;
    int encoding = 0;

    int keyframeCount;
    int componentCount;
    int[] times;
    float[] values;

    KeyframeSequence() {
    }

    public KeyframeSequence(int numKeyframes, int numComponents,
                            int interpolation) {
        if (numKeyframes <= 0 || numComponents <= 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad sequence size"));
        }
        this.keyframeCount = numKeyframes;
        this.componentCount = numComponents;
        this.interpolation = interpolation;
        this.times = new int[numKeyframes];
        this.values = new float[numKeyframes * numComponents];
    }

    public void setKeyframe(int index, int time, float[] value) {
        if (index < 0 || index >= keyframeCount) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        if (value == null || value.length < componentCount) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("bad keyframe value"));
        }
        times[index] = time;
        System.arraycopy(value, 0, values, index * componentCount,
                         componentCount);
        if (time > duration) {
            duration = time;
        }
    }

    public int getKeyframeCount() {
        return keyframeCount;
    }

    public int getComponentCount() {
        return componentCount;
    }

    public int getKeyframe(int index, float[] value) {
        if (index < 0 || index >= keyframeCount) {
            throw com.sun.m3g.M3gTrace.fail(new IndexOutOfBoundsException());
        }
        if (value != null) {
            System.arraycopy(values, index * componentCount, value, 0,
                             componentCount);
        }
        return times[index];
    }

    public void setDuration(int d) {
        if (d < 0) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("negative duration"));
        }
        duration = d;
    }

    public int getDuration() {
        return duration;
    }

    public void setRepeatMode(int mode) {
        repeatMode = mode;
    }

    public int getRepeatMode() {
        return repeatMode;
    }

    public void setValidRange(int first, int last) {
        validRangeStart = first;
        validRangeEnd = last;
    }

    public int getValidRangeFirst() {
        return validRangeStart;
    }

    public int getValidRangeLast() {
        return validRangeEnd;
    }

    public int getInterpolationType() {
        return interpolation;
    }
}
