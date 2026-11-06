/*
 * Criteria.java - JSR-179 (see Coordinates.java for the scope).
 *
 * All setters are accepted and ignored: the port has a single, always
 * available provider whose properties no game can influence.
 */
package javax.microedition.location;

public class Criteria {

    public static final int NO_REQUIREMENT = 0;
    public static final int POWER_USAGE_LOW = 1;
    public static final int POWER_USAGE_MEDIUM = 2;
    public static final int POWER_USAGE_HIGH = 3;

    private int horizontalAccuracy = NO_REQUIREMENT;
    private int verticalAccuracy = NO_REQUIREMENT;
    private int preferredResponseTime = NO_REQUIREMENT;

    public Criteria() {
    }

    public void setHorizontalAccuracy(int accuracy) {
        if (accuracy < 0) {
            throw new IllegalArgumentException("negative accuracy");
        }
        horizontalAccuracy = accuracy;
    }

    public void setVerticalAccuracy(int accuracy) {
        if (accuracy < 0) {
            throw new IllegalArgumentException("negative accuracy");
        }
        verticalAccuracy = accuracy;
    }

    public void setPreferredResponseTime(int time) {
        preferredResponseTime = time;
    }

    public void setPreferredPowerConsumption(int level) {
        /* accepted, ignored */
    }

    public void setCostAllowed(boolean costAllowed) {
        /* accepted, ignored */
    }

    public void setSpeedAndCourseRequired(boolean speedAndCourseRequired) {
        /* accepted, ignored */
    }

    public void setAltitudeRequired(boolean altitudeRequired) {
        /* accepted, ignored */
    }

    public void setAddressInfoRequired(boolean addressInfoRequired) {
        /* accepted, ignored */
    }

    /* Getters kept so a MIDlet that reads back what it set sees it. */
    public int getHorizontalAccuracy() {
        return horizontalAccuracy;
    }

    public int getVerticalAccuracy() {
        return verticalAccuracy;
    }

    public int getPreferredResponseTime() {
        return preferredResponseTime;
    }
}
