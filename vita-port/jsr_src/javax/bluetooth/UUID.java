/*
 * UUID.java - JSR-82 (see BluetoothStateException.java for the scope note).
 *
 * A Bluetooth UUID: either the 4-hex-digit short form or the full 32-hex-digit
 * form. Values are normalised to upper case so equals() compares like the
 * specification requires.
 */
package javax.bluetooth;

public class UUID {

    private final String value;

    public UUID(String uuidValue, boolean shortUUID) {
        if (uuidValue == null) {
            throw new NullPointerException("uuidValue");
        }
        String v = uuidValue;
        if (shortUUID) {
            if (v.length() != 4) {
                throw new IllegalArgumentException("short UUID needs 4 hex digits");
            }
        } else {
            if (v.length() != 32) {
                throw new IllegalArgumentException("UUID needs 32 hex digits");
            }
        }
        this.value = v.toUpperCase();
    }

    public boolean equals(Object o) {
        if (!(o instanceof UUID)) {
            return false;
        }
        return value.equals(((UUID) o).value);
    }

    public int hashCode() {
        return value.hashCode();
    }

    public String toString() {
        return value;
    }
}
