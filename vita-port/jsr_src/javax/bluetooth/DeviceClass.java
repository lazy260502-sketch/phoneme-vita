/*
 * DeviceClass.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 *
 * The 24-bit class-of-device record: bits 0-1 format, bits 2-7 minor, bits 8-12
 * major, bits 13-23 service classes.
 */
package javax.bluetooth;

public class DeviceClass {

    private final int record;

    public DeviceClass(int record) {
        if ((record & 0xFF000000) != 0) {
            throw new IllegalArgumentException("record is 24 bits");
        }
        this.record = record;
    }

    public int getServiceClasses() {
        return (record >> 13) & 0x7FF;
    }

    public int getMajorDeviceClass() {
        return (record >> 8) & 0x1F;
    }

    public int getMinorDeviceClass() {
        return record & 0xFC;
    }

    public boolean equals(Object o) {
        return (o instanceof DeviceClass) && ((DeviceClass) o).record == record;
    }

    public int hashCode() {
        return record;
    }
}
