/*
 * Orientation.java - JSR-179 (see Coordinates.java for the scope).
 */
package javax.microedition.location;

/** Device orientation as reported by a compass; static here. */
public class Orientation {

    /** Increasing azimuth is clockwise. */
    public static final int CLOCKWISE = 1;
    /** Increasing azimuth is counter-clockwise. */
    public static final int COUNTERCLOCKWISE = -1;

    private final float azimuth;
    private final boolean magnetic;
    private final float pitch;
    private final float roll;

    public Orientation(float azimuth, boolean isMagnetic, float pitch,
                       float roll) {
        this.azimuth = azimuth;
        this.magnetic = isMagnetic;
        this.pitch = pitch;
        this.roll = roll;
    }

    /** Degrees clockwise from north (0..360). */
    public float getAzimuth() {
        return azimuth;
    }

    public boolean isMagnetic() {
        return magnetic;
    }

    /** -90..90 degrees, positive = nose up. */
    public float getPitch() {
        return pitch;
    }

    /** -180..180 degrees, positive = rolled right. */
    public float getRoll() {
        return roll;
    }
}
