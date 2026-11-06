/*
 * JSR-82 (Bluetooth API) for the PS Vita port - SCOPE
 * ===================================================
 * A Vita has no Bluetooth radio. This implementation exists so that a MIDlet
 * which imports javax.bluetooth / opens "btspp://" does not die on
 * ClassNotFoundException, and so that a single-player path keeps working:
 *   - LocalDevice reports a fixed address and name;
 *   - the DiscoveryAgent finds nothing (no peers exist);
 *   - "btspp://" connections are LOOPBACK: everything written is readable from
 *     the same connection, and all btspp connections share one byte bus so two
 *     connections can talk to each other on the same console.
 *
 * NOTE ON SIGNATURES: the JSR-82 Javadoc is not available in the local mirror
 * (nikita36078's docs stop at other JSRs), so these signatures follow the
 * published JSR-82 API from memory. If a game throws NoSuchMethodError, compare
 * against the real specification before inventing anything.
 */
package javax.bluetooth;

import java.io.IOException;

/** Thrown when the local Bluetooth device cannot satisfy a request. */
public class BluetoothStateException extends IOException {

    public BluetoothStateException() {
        super();
    }

    public BluetoothStateException(String msg) {
        super(msg);
    }
}
