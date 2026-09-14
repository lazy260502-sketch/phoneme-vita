/*
 * JSR 75 (PDA Optional Packages) public API for the PS Vita port.
 *
 * Vita-local subsystem: the class name and its super class follow the
 * JSR 75 specification, the implementation is original.
 */
package javax.microedition.io.file;

import java.io.IOException;

/**
 * Thrown when an operation is attempted on a FileConnection that has
 * been closed.
 */
public class ConnectionClosedException extends IOException {

    /**
     * Constructs an exception without a detail message.
     */
    public ConnectionClosedException() {
        super();
    }

    /**
     * Constructs an exception with a detail message.
     *
     * @param message the detail message
     */
    public ConnectionClosedException(String message) {
        super(message);
    }
}
