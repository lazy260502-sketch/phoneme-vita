/*
 * JSR 75 (PDA Optional Packages) public API for the PS Vita port.
 *
 * Vita-local subsystem: the class name and its super class follow the
 * JSR 75 specification, the implementation is original.
 */
package javax.microedition.io.file;

/**
 * Thrown when a FileConnection method is used in an access mode the
 * connection was not opened with, for example writing through a
 * connection opened for reading only.
 */
public class IllegalModeException extends RuntimeException {

    /**
     * Constructs an exception without a detail message.
     */
    public IllegalModeException() {
        super();
    }

    /**
     * Constructs an exception with a detail message.
     *
     * @param message the detail message
     */
    public IllegalModeException(String message) {
        super(message);
    }
}
