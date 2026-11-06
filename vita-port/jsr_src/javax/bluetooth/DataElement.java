/*
 * DataElement.java - JSR-82 (see BluetoothStateException.java for the scope
 * note).
 *
 * Minimal SDP data element: the type constants, the five constructors a
 * service record realistically needs, getValue()/getSize() and the two
 * sequence mutators. There is deliberately NO DataElement(int value)
 * constructor: the specification uses the int overload for the TYPE
 * (NULL/DATSEQ/DATALT), and having both would make `new DataElement(5)`
 * ambiguous at the call site. Use DataElement((long) value) for integers.
 */
package javax.bluetooth;

import java.util.Vector;

public class DataElement {

    public static final int NULL = 0;
    public static final int BOOL = 1;
    public static final int INT_1 = 2;
    public static final int INT_2 = 3;
    public static final int INT_4 = 4;
    public static final int INT_8 = 8;
    public static final int INT_16 = 16;
    public static final int UUID = 24;
    public static final int STRING = 32;
    public static final int DATSEQ = 48;
    public static final int DATALT = 49;
    public static final int URL = 64;

    private int type;
    private Object value;

    /** NULL, DATSEQ or DATALT. */
    public DataElement(int valueType) {
        if (valueType != NULL && valueType != DATSEQ && valueType != DATALT) {
            throw new IllegalArgumentException(
                "use DataElement((long) n) for integer values");
        }
        type = valueType;
        if (valueType == DATSEQ || valueType == DATALT) {
            value = new Vector();
        }
    }

    public DataElement(boolean bool) {
        type = BOOL;
        value = new Boolean(bool);
    }

    public DataElement(long v) {
        if (v >= -128L && v <= 127L) {
            type = INT_1;
        } else if (v >= -32768L && v <= 32767L) {
            type = INT_2;
        } else if (v >= -2147483648L && v <= 2147483647L) {
            type = INT_4;
        } else {
            type = INT_8;
        }
        value = new Long(v);
    }

    public DataElement(String string, boolean isURL) {
        if (string == null) {
            throw new NullPointerException("string");
        }
        type = isURL ? URL : STRING;
        value = string;
    }

    public DataElement(UUID uuid) {
        if (uuid == null) {
            throw new NullPointerException("uuid");
        }
        type = UUID;
        value = uuid;
    }

    public int getDataType() {
        return type;
    }

    public Object getValue() {
        return value;
    }

    public int getSize() {
        if (value instanceof Vector) {
            return ((Vector) value).size();
        }
        return 1;
    }

    public void addElement(DataElement elem) {
        seq().addElement(elem);
    }

    public void insertElementAt(DataElement elem, int index) {
        seq().insertElementAt(elem, index);
    }

    public boolean removeElement(DataElement elem) {
        Vector v = seq();
        return v.removeElement(elem);
    }

    private Vector seq() {
        if (type != DATSEQ && type != DATALT) {
            throw new ClassCastException("not a DATSEQ/DATALT element");
        }
        return (Vector) value;
    }
}
