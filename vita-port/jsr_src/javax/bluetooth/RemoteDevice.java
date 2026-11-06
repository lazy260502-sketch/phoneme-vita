/*
 * RemoteDevice.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 *
 * A peer device. Addresses are the 12-digit hex form the specification uses
 * (no colons). Nothing can actually be reached, so the security helpers report
 * the permissive answer a loopback connection expects.
 */
package javax.bluetooth;

import java.io.IOException;

import javax.microedition.io.Connection;

public class RemoteDevice {

    private final String address;

    private RemoteDevice(String address) {
        this.address = address;
    }

    public static RemoteDevice getRemoteDevice(String address) {
        if (address == null) {
            throw new NullPointerException("address");
        }
        if (address.length() != 12) {
            throw new IllegalArgumentException("address must be 12 hex digits");
        }
        for (int i = 0; i < 12; i++) {
            char c = address.charAt(i);
            boolean hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
                       || (c >= 'A' && c <= 'F');
            if (!hex) {
                throw new IllegalArgumentException("address must be hex");
            }
        }
        return new RemoteDevice(address.toUpperCase());
    }

    public String getBluetoothAddress() {
        return address;
    }

    public String getFriendlyName(boolean alwaysAsk) throws IOException {
        return "peer-" + address;
    }

    public boolean isTrustedDevice() {
        return false;
    }

    public boolean authorize(Connection conn) throws IOException {
        return true;
    }

    public boolean encrypt(Connection conn, boolean on) throws IOException {
        return true;
    }

    public boolean isAuthenticated() {
        return false;
    }

    public boolean isEncrypted() {
        return false;
    }

    public boolean equals(Object o) {
        return (o instanceof RemoteDevice)
            && address.equals(((RemoteDevice) o).address);
    }

    public int hashCode() {
        return address.hashCode();
    }
}
