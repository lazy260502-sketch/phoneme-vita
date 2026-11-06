/*
 * QualifiedCoordinates.java - JSR-179 (see Coordinates.java for the scope).
 */
package javax.microedition.location;

/** Coordinates with the accuracy the provider claims for them. */
public class QualifiedCoordinates extends Coordinates {

    private float horizontalAccuracy;
    private float verticalAccuracy;

    public QualifiedCoordinates(double latitude, double longitude,
                                float altitude, float horizontalAccuracy,
                                float verticalAccuracy) {
        super(latitude, longitude, altitude);
        this.horizontalAccuracy = horizontalAccuracy;
        this.verticalAccuracy = verticalAccuracy;
    }

    public float getHorizontalAccuracy() {
        return horizontalAccuracy;
    }

    public float getVerticalAccuracy() {
        return verticalAccuracy;
    }

    public void setHorizontalAccuracy(float accuracy) {
        this.horizontalAccuracy = accuracy;
    }

    public void setVerticalAccuracy(float accuracy) {
        this.verticalAccuracy = accuracy;
    }
}
