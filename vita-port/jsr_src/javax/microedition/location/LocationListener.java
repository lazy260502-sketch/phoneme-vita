/*
 * LocationListener.java - JSR-179 (see Coordinates.java for the scope).
 */
package javax.microedition.location;

/** Receives position updates from a {@link LocationProvider}. */
public interface LocationListener {

    void locationUpdated(LocationProvider provider, Location location);

    void providerStateChanged(LocationProvider provider, int newState);
}
