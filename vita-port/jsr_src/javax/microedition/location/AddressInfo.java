/*
 * AddressInfo.java - JSR-179 (see Coordinates.java for the scope).
 *
 * Minimal: field values are stored one per AddressInfo, which is enough for a
 * MIDlet that reads a street or a city out of a location it got from the fixed
 * provider. There is no reverse geocoding - no address data exists on the
 * console.
 */
package javax.microedition.location;

public abstract class AddressInfo {

    public static final int EXTENSION = 1;
    public static final int STREET = 2;
    public static final int POSTAL_CODE = 3;
    public static final int CITY = 4;
    public static final int COUNTY = 5;
    public static final int STATE = 6;
    public static final int COUNTRY = 7;
    public static final int COUNTRY_CODE = 8;
    public static final int DISTRICT = 9;
    public static final int BUILDING_NAME = 10;
    public static final int FLOOR = 11;
    public static final int ROOM = 12;
    public static final int PHONE_NUMBER = 13;

    protected AddressInfo() {
    }

    public abstract String getField(int field);

    public abstract void setField(int field, String value);

    /** One-field AddressInfo, as the specification's factory requires. */
    public static AddressInfo createField(int field, String value) {
        return new VitaAddressInfo(field, value);
    }
}

/** Package-private: the single-field implementation behind createField(). */
final class VitaAddressInfo extends AddressInfo {

    private int field;
    private String value;

    VitaAddressInfo(int field, String value) {
        this.field = field;
        this.value = value;
    }

    public String getField(int f) {
        return (f == field) ? value : null;
    }

    public void setField(int f, String v) {
        if (f == field) {
            value = v;
        }
    }
}
