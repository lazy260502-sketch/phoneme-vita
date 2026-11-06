package javax.microedition.m3g;

/**
 * Playback state shared by the tracks of one animation.
 */
public class AnimationController extends Object3D {

    private float speed = 0.0f;
    private float weight = 1.0f;
    private int position = 0;
    private boolean active = false;
    private int activationTime = 0;
    private int deactivationTime = 0;

    public AnimationController() {
    }

    AnimationController(boolean placeholder) {
    }

    public void setActiveInterval(int start, int end) {
        active = end > start;
    }

    public int getActiveIntervalStart() {
        return 0;
    }

    public int getActiveIntervalEnd() {
        return 0;
    }

    public void setSpeed(float speed, int worldTime) {
        this.speed = speed;
        active = speed != 0.0f;
    }

    public float getSpeed() {
        return speed;
    }

    public void setPosition(float position, int worldTime) {
        this.position = (int)(position * 1000.0f);
    }

    public void setWeight(float weight) {
        if (weight < 0.0f || weight > 1.0f) {
            throw com.sun.m3g.M3gTrace.fail(new IllegalArgumentException("weight out of range"));
        }
        this.weight = weight;
    }

    public float getWeight() {
        return weight;
    }

    /** Loader path: state as stored in the file. */
    void loadState(float speed, float weight, int start, int end) {
        this.speed = speed;
        this.weight = weight;
        this.activationTime = start;
        this.deactivationTime = end;
        this.active = true;
    }

    int getActivationTime() {
        return activationTime;
    }

    int getDeactivationTime() {
        return deactivationTime;
    }

    void setPlaybackPosition(int ms) {
        this.position = ms;
    }

    boolean isActive() {
        return active && weight > 0.0f;
    }

    /** Clamped-by-duration position in milliseconds. */
    /** Advances to an absolute world time; position = speed * time. */
    void advanceTo(int worldTime) {
        position = (int)(speed * (float)worldTime);
    }

    int getPosition(int duration) {
        if (position < 0) {
            return 0;
        }
        if (duration > 0 && position > duration) {
            return duration;
        }
        return position;
    }
}
