/*
 * ValueOutOfRangeException.java - JSR-82 (see BluetoothStateException.java for
 * the scope note).
 */
package javax.bluetooth;

/** Thrown when a DataElement value does not fit its declared type. */
public class ValueOutOfRangeException extends RuntimeException {

    public ValueOutOfRangeException() {
        super();
    }

    public ValueOutOfRangeException(String msg) {
        super(msg);
    }
}
