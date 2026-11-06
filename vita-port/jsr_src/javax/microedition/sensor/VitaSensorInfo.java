/*
 * VitaSensorInfo.java - SensorInfo + ChannelInfo implementation
 * (see VitaSensor.java for the data-source note).
 */
package javax.microedition.sensor;

final class VitaSensorInfo implements SensorInfo {

    static final int KIND_ACCEL = 0;
    static final int KIND_GYRO = 1;

    final int kind;
    private final String quantity;
    private final String contextType;
    private final ChannelInfo[] channels;
    private final String url;

    VitaSensorInfo(String quantity, String contextType, String[] names,
                   int kind) {
        this.quantity = quantity;
        this.contextType = contextType;
        this.kind = kind;
        this.channels = new ChannelInfo[names.length];
        for (int i = 0; i < names.length; i++) {
            channels[i] = new VitaChannelInfo(names[i], kind);
        }
        this.url = "sensor://" + quantity + ";contextType=" + contextType
                 + ";model=" + VitaSensor.MODEL;
    }

    public ChannelInfo[] getChannelInfos() {
        return channels;
    }

    public int getConnectionType() {
        return CONN_EMBEDDED;
    }

    public String getContextType() {
        return contextType;
    }

    public String getDescription() {
        return "PS Vita " + quantity + " (synthetic)";
    }

    public int getMaxBufferSize() {
        return VitaSensor.MAX_BUFFER;
    }

    public String getModel() {
        return VitaSensor.MODEL;
    }

    public String[] getPropertyNames() {
        return new String[] { PROP_VENDOR, PROP_VERSION, PROP_MAX_RATE };
    }

    public Object getProperty(String name) {
        if (name == null) {
            throw new NullPointerException("name");
        }
        if (PROP_VENDOR.equals(name)) {
            return "J2ME Player";
        }
        if (PROP_VERSION.equals(name)) {
            return "1.0";
        }
        if (PROP_MAX_RATE.equals(name)) {
            /* 50 Hz, which is what the pump delivers. */
            return new Integer(50);
        }
        throw new IllegalArgumentException("unknown property " + name);
    }

    public String getQuantity() {
        return quantity;
    }

    public String getUrl() {
        return url;
    }

    public boolean isAvailable() {
        return true;
    }

    public boolean isAvailabilityPushSupported() {
        return false;   /* embedded, always on */
    }

    public boolean isConditionPushSupported() {
        return false;
    }
}

/** Per-channel metadata; the axis index is implicit in the array order. */
final class VitaChannelInfo implements ChannelInfo {

    private final String name;
    private final Unit unit;
    private final MeasurementRange[] ranges;

    VitaChannelInfo(String name, int kind) {
        boolean accel = (kind == VitaSensorInfo.KIND_ACCEL);
        this.name = name;
        this.unit = Unit.getUnit(accel ? "m/s^2" : "rad/s");
        /* +-2 g and +-2000 deg/s, the console's own ranges, in basic units. */
        double limit = accel ? 19.62 : 34.91;
        this.ranges = new MeasurementRange[] {
            new MeasurementRange(-limit, limit, 0.01)
        };
    }

    public float getAccuracy() {
        return 1.0f;
    }

    public int getDataType() {
        return TYPE_DOUBLE;
    }

    public MeasurementRange[] getMeasurementRanges() {
        return ranges;
    }

    public String getName() {
        return name;
    }

    public int getScale() {
        return 0;
    }

    public Unit getUnit() {
        return unit;
    }
}
