/*
 * VitaServiceRecord.java - the port's ServiceRecord (see
 * BluetoothStateException.java for the scope note).
 *
 * It is a plain attribute table; nothing is ever sent over the air, so
 * populateRecord() has nothing to fetch and reports success.
 */
package javax.bluetooth;

import java.util.Enumeration;
import java.util.Hashtable;
import java.util.Vector;

final class VitaServiceRecord implements ServiceRecord {

    private final Hashtable attrs = new Hashtable();
    private final RemoteDevice host;
    private final String url;
    private int classes;

    VitaServiceRecord(RemoteDevice host, String url) {
        this.host = host;
        this.url = url;
    }

    public int[] getAttributeIDs() {
        Vector v = new Vector();
        Enumeration e = attrs.keys();
        while (e.hasMoreElements()) {
            v.addElement(e.nextElement());
        }
        int[] r = new int[v.size()];
        for (int i = 0; i < r.length; i++) {
            r[i] = ((Integer) v.elementAt(i)).intValue();
        }
        return r;
    }

    public DataElement getAttributeValue(int attrID) {
        return (DataElement) attrs.get(new Integer(attrID));
    }

    public String getConnectionURL(int requiredSecurity, boolean mustBeMaster) {
        return url;
    }

    public boolean populateRecord(int[] attrIDs) {
        return true;        /* loopback: every attribute is already local */
    }

    public RemoteDevice getHostDevice() {
        return host;
    }

    public boolean setAttributeValue(int attrID, DataElement attrValue)
            throws ServiceRegistrationException {
        if (attrID < 0 || attrID > 0xFFFF) {
            throw new IllegalArgumentException("attribute ID out of range");
        }
        Integer key = new Integer(attrID);
        if (attrValue == null) {
            attrs.remove(key);
        } else {
            attrs.put(key, attrValue);
        }
        return true;
    }

    public boolean setDeviceServiceClasses(int c) {
        classes = c;
        return true;
    }
}
