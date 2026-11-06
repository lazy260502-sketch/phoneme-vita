/*
 * BluetoothConnectionException.java - JSR-82 (see BluetoothStateException.java
 * for the scope note).
 */
package javax.bluetooth;

import java.io.IOException;

/** Thrown when a connection attempt fails. */
public class BluetoothConnectionException extends IOException {

    public static final int UNKNOWN_PSM = 1;
    public static final int SECURITY_BLOCK = 2;
    public static final int NO_RESOURCES = 3;
    public static final int FAILURE = 4;
    public static final int CONNECTION_TIMEOUT = 5;

    private final int status;

    public BluetoothConnectionException(int error) {
        super();
        if (error < UNKNOWN_PSM || error > CONNECTION_TIMEOUT) {
            throw new IllegalArgumentException("invalid status");
        }
        this.status = error;
    }

    public BluetoothConnectionException(int error, String msg) {
        super(msg);
        if (error < UNKNOWN_PSM || error > CONNECTION_TIMEOUT) {
            throw new IllegalArgumentException("invalid status");
        }
        this.status = error;
    }

    public int getStatus() {
        return status;
    }
}
