/*
 * DiscoveryAgent.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 *
 * There is no radio, so an inquiry finds nothing and a service search reports
 * SERVICE_SEARCH_NO_RECORDS. Both callbacks run on a one-shot thread: a MIDlet
 * that blocks its own thread waiting for the callback would deadlock if the
 * callback were delivered synchronously.
 */
package javax.bluetooth;

public abstract class DiscoveryAgent {

    public static final int NOT_DISCOVERABLE = 0;
    public static final int GIAC = 0x9E8B33;
    public static final int LIAC = 0x9E8B00;
    public static final int CACHED = 0x00;
    public static final int PREKNOWN = 0x01;

    public static final int INQUIRY_COMPLETED = 0;
    public static final int INQUIRY_TERMINATED = 5;
    public static final int INQUIRY_ERROR = 7;

    public static final int SERVICE_SEARCH_COMPLETED = 1;
    public static final int SERVICE_SEARCH_TERMINATED = 2;
    public static final int SERVICE_SEARCH_ERROR = 3;
    public static final int SERVICE_SEARCH_NO_RECORDS = 4;
    public static final int SERVICE_SEARCH_DEVICE_NOT_REACHABLE = 6;

    protected DiscoveryAgent() {
    }

    public abstract RemoteDevice[] retrieveDevices(int option);

    public abstract boolean startInquiry(int accessCode,
                                        DiscoveryListener listener)
            throws BluetoothStateException;

    public abstract boolean cancelInquiry(DiscoveryListener listener);

    public abstract int searchServices(int[] attrSet, UUID[] uuidSet,
                                       RemoteDevice btDev,
                                       DiscoveryListener listener)
            throws BluetoothStateException;

    public abstract boolean cancelServiceSearch(int transID);

    public abstract String selectService(UUID uuid, int security,
                                         boolean master)
            throws BluetoothStateException;
}

/** The port's only agent: completes every request with "nothing found". */
final class VitaDiscoveryAgent extends DiscoveryAgent {

    private int nextTrans = 1;

    public RemoteDevice[] retrieveDevices(int option) {
        return null;        /* specification: null when no devices are known */
    }

    public boolean startInquiry(int accessCode, final DiscoveryListener l)
            throws BluetoothStateException {
        if (l == null) {
            throw new NullPointerException("listener");
        }
        new Thread(new Runnable() {
            public void run() {
                l.inquiryCompleted(INQUIRY_COMPLETED);
            }
        }).start();
        return true;
    }

    public boolean cancelInquiry(DiscoveryListener listener) {
        return false;       /* nothing was running */
    }

    public int searchServices(int[] attrSet, UUID[] uuidSet,
                              RemoteDevice btDev, final DiscoveryListener l)
            throws BluetoothStateException {
        if (l == null) {
            throw new NullPointerException("listener");
        }
        final int trans = nextTrans++;
        new Thread(new Runnable() {
            public void run() {
                l.serviceSearchCompleted(trans, SERVICE_SEARCH_NO_RECORDS);
            }
        }).start();
        return trans;
    }

    public boolean cancelServiceSearch(int transID) {
        return false;
    }

    public String selectService(UUID uuid, int security, boolean master)
            throws BluetoothStateException {
        throw new BluetoothStateException("no Bluetooth radio");
    }
}
