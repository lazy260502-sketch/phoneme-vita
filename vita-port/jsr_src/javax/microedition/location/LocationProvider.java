/*
 * LocationProvider.java - JSR-179 (see Coordinates.java for the scope).
 *
 * The Vita has no GPS. getInstance() always returns the same fixed-position
 * provider (VitaLocationProvider), whatever Criteria the MIDlet passes, so
 * code that guards with Class.forName / getInstance works instead of dying.
 */
package javax.microedition.location;

public abstract class LocationProvider {

    public static final int AVAILABLE = 1;
    public static final int TEMPORARILY_UNAVAILABLE = 2;
    public static final int OUT_OF_SERVICE = 3;

    private static LocationProvider instance;

    protected LocationProvider() {
    }

    /**
     * @param criteria ignored - the port has exactly one provider
     * @return the shared provider instance
     */
    public static LocationProvider getInstance(Criteria criteria)
            throws LocationException {
        if (instance == null) {
            instance = new VitaLocationProvider();
        }
        return instance;
    }

    public abstract Location getLocation(int timeout)
            throws LocationException, InterruptedException;

    public abstract Location getLastKnownLocation();

    public abstract void setLocationListener(LocationListener listener,
                                             int interval, int timeout,
                                             int maxAge);

    public abstract int getState();

    public abstract void reset();
}
