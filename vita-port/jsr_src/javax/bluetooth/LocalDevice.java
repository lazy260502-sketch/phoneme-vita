/*
 * LocalDevice.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 *
 * A fixed identity ("001122334455" / "PS Vita") that is always powered and
 * discoverable, so a MIDlet can print "my ID" without crashing. Records are
 * kept per notifier exactly as the specification describes.
 */
package javax.bluetooth;

import java.util.Hashtable;

import javax.microedition.io.Connection;

public class LocalDevice {

    public static final int LIAC = 0x9E8B00;
    public static final int GIAC = 0x9E8B33;

    private static LocalDevice instance;

    private final DiscoveryAgent agent = new VitaDiscoveryAgent();
    private final Hashtable records = new Hashtable();
    private int discoverable = DiscoveryAgent.GIAC;

    private LocalDevice() {
    }

    public static synchronized LocalDevice getLocalDevice()
            throws BluetoothStateException {
        if (instance == null) {
            instance = new LocalDevice();
        }
        return instance;
    }

    public static boolean isPowerOn() {
        return true;
    }

    public String getBluetoothAddress() {
        return "001122334455";
    }

    public String getFriendlyName() throws BluetoothStateException {
        return "PS Vita";
    }

    public DiscoveryAgent getDiscoveryAgent() {
        return agent;
    }

    public boolean setDiscoverable(int mode) throws BluetoothStateException {
        if (mode != DiscoveryAgent.NOT_DISCOVERABLE
                && mode != DiscoveryAgent.GIAC
                && mode != DiscoveryAgent.LIAC) {
            throw new IllegalArgumentException("invalid mode");
        }
        discoverable = mode;
        return true;
    }

    public int getDiscoverable() throws BluetoothStateException {
        return discoverable;
    }

    /** The record a notifier registered, or null (loopback keeps none). */
    public ServiceRecord getRecord(Connection notifier) {
        if (notifier == null) {
            throw new NullPointerException("notifier");
        }
        return (ServiceRecord) records.get(notifier);
    }

    public void updateRecord(ServiceRecord srvRecord)
            throws ServiceRegistrationException {
        if (srvRecord == null) {
            throw new NullPointerException("srvRecord");
        }
        /* Nothing is published: there is no radio to advertise on. */
    }

    public String getProperty(String property) {
        if ("bluetooth.spec.version".equals(property)) {
            return "1.1";
        }
        if ("bluetooth.connected.devices.max".equals(property)) {
            return "7";
        }
        return null;
    }
}
