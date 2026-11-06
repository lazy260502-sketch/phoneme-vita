/*
 * Location.java - JSR-179 (see Coordinates.java for the scope).
 */
package javax.microedition.location;

/** One position fix: coordinates plus time, speed, course and address. */
public class Location {

    private QualifiedCoordinates coords;
    private long timestamp;
    private float speed;
    private float course;
    private AddressInfo addressInfo;
    private boolean valid;

    protected Location() {
    }

    /** Filled in by VitaLocationProvider (same package). */
    void vitaInit(QualifiedCoordinates c, long timestamp, float speed,
                  float course) {
        this.coords = c;
        this.timestamp = timestamp;
        this.speed = speed;
        this.course = course;
        this.valid = true;
    }

    public QualifiedCoordinates getQualifiedCoordinates() {
        return coords;
    }

    public float getSpeed() {
        return speed;
    }

    /** Course in degrees clockwise from north. */
    public float getCourse() {
        return course;
    }

    /** Milliseconds since the epoch. */
    public long getTimeStamp() {
        return timestamp;
    }

    public AddressInfo getAddressInfo() {
        return addressInfo;
    }

    public boolean isValid() {
        return valid;
    }
}
