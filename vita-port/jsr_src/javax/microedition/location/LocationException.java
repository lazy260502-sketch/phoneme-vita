/*
 * LocationException.java - JSR-179 (see Coordinates.java for the scope).
 */
package javax.microedition.location;

/** Thrown when a location operation cannot be completed. */
public class LocationException extends Exception {

    public LocationException() {
        super();
    }

    public LocationException(String message) {
        super(message);
    }
}
