/*
 * DiscoveryListener.java - JSR-82 (see BluetoothStateException.java for the
 * scope note).
 */
package javax.bluetooth;

/** Receives inquiry and service-search results. */
public interface DiscoveryListener {

    void deviceDiscovered(RemoteDevice btDevice, DeviceClass cod);

    void servicesDiscovered(int transID, ServiceRecord[] servRecord);

    void serviceSearchCompleted(int transID, int respCode);

    void inquiryCompleted(int discType);
}
