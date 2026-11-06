/*
 * ServiceRegistrationException.java - JSR-82 (see BluetoothStateException.java
 * for the scope note).
 */
package javax.bluetooth;

import java.io.IOException;

/** Thrown when a service record cannot be registered or updated. */
public class ServiceRegistrationException extends IOException {

    public ServiceRegistrationException() {
        super();
    }

    public ServiceRegistrationException(String msg) {
        super(msg);
    }
}
