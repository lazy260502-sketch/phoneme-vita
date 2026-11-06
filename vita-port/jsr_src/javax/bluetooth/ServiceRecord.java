/*
 * ServiceRecord.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 */
package javax.bluetooth;

import java.io.IOException;

/** One SDP record: attribute IDs to DataElement values plus the connect URL. */
public interface ServiceRecord {

    int NOAUTHENTICATE_NOENCRYPT = 0;
    int AUTHENTICATE_NOENCRYPT = 1;
    int AUTHENTICATE_ENCRYPT = 2;

    int[] getAttributeIDs();

    DataElement getAttributeValue(int attrID);

    String getConnectionURL(int requiredSecurity, boolean mustBeMaster);

    boolean populateRecord(int[] attrIDs) throws IOException;

    RemoteDevice getHostDevice();

    boolean setAttributeValue(int attrID, DataElement attrValue)
            throws ServiceRegistrationException;

    boolean setDeviceServiceClasses(int classes);
}
