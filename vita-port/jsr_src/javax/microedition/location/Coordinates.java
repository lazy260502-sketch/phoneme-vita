/*
 * Coordinates.java - JSR-179 (Location API) for the PS Vita port.
 *
 * The port ships a MINIMAL JSR-179: enough that
 *   Class.forName("javax.microedition.location.LocationProvider")
 * succeeds and a game can read a plausible position, instead of dying on
 * ClassNotFoundException the way it does with no location API at all.
 *
 * There is no GPS on a Vita. LocationProvider therefore reports a fixed
 * position (see VitaLocationProvider.java for the value and why).
 *
 * Only the parts of the specification a MIDlet realistically calls are
 * implemented; anything else is documented as a stub in PROJECT_MEMORY.md.
 */
package javax.microedition.location;

/**
 * Coordinates of a point on Earth (WGS84), in degrees and metres.
 */
public class Coordinates {

    private final double latitude;
    private final double longitude;
    private float altitude;

    public Coordinates(double latitude, double longitude, float altitude) {
        if (latitude < -90.0 || latitude > 90.0) {
            throw new IllegalArgumentException("latitude out of range");
        }
        if (longitude < -180.0 || longitude > 180.0) {
            throw new IllegalArgumentException("longitude out of range");
        }
        this.latitude = latitude;
        this.longitude = longitude;
        this.altitude = altitude;
    }

    public double getLatitude() {
        return latitude;
    }

    public double getLongitude() {
        return longitude;
    }

    public float getAltitude() {
        return altitude;
    }

    public void setAltitude(float altitude) {
        this.altitude = altitude;
    }

    /**
     * Distance in metres. CLDC 1.1's Math has no atan2/asin (only
     * sin/cos/tan/sqrt), so the haversine form is out; this is the
     * equirectangular approximation, which is exact enough (< 0.1 % error) for
     * the few-kilometre ranges a game cares about and cheap besides.
     */
    public float distance(Coordinates to) {
        if (to == null) {
            throw new NullPointerException();
        }
        double r = Math.PI / 180.0;
        double dLat = (to.latitude - latitude) * r;
        double dLon = (to.longitude - longitude) * r;
        double midLat = ((latitude + to.latitude) / 2.0) * r;
        double x = dLon * Math.cos(midLat);
        return (float) (6371000.0 * Math.sqrt(x * x + dLat * dLat));
    }

    /** Initial bearing towards {@code to}, in degrees clockwise from north. */
    public float azimuthTo(Coordinates to) {
        if (to == null) {
            throw new NullPointerException();
        }
        double r = Math.PI / 180.0;
        double lat1 = latitude * r;
        double lat2 = to.latitude * r;
        double dLon = (to.longitude - longitude) * r;
        double y = Math.sin(dLon) * Math.cos(lat2);
        double x = Math.cos(lat1) * Math.sin(lat2)
                 - Math.sin(lat1) * Math.cos(lat2) * Math.cos(dLon);
        double deg = atan2(y, x) * (180.0 / Math.PI);
        if (deg < 0.0) {
            deg += 360.0;
        }
        return (float) deg;
    }

    /**
     * atan2 for a CLDC 1.1 Math class, which has no such function. Polynomial
     * approximation of atan on [0,1] (|error| < 1e-4 rad), folded into the four
     * quadrants - plenty for a compass bearing.
     */
    private static double atan2(double y, double x) {
        if (x == 0.0 && y == 0.0) {
            return 0.0;
        }
        double ax = Math.abs(x);
        double ay = Math.abs(y);
        double a = (ax > ay) ? (ay / ax) : (ax / ay);   /* 0..1 */
        double s = a * a;
        double r = a * (0.9998660 + s * (-0.3302995 + s * (0.1801410
                 + s * (-0.0851330 + s * 0.0208351))));
        if (ax <= ay) {
            r = Math.PI / 2.0 - r;
        }
        if (x < 0.0) {
            r = Math.PI - r;
        }
        if (y < 0.0) {
            r = -r;
        }
        return r;
    }
}
