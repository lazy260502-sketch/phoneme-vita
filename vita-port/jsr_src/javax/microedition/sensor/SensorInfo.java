/*
 * SensorInfo.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/** Everything a MIDlet can learn about a sensor without opening it. */
public interface SensorInfo {

    int CONN_EMBEDDED = 1;
    int CONN_REMOTE = 2;
    int CONN_SHORT_RANGE_WIRELESS = 4;
    int CONN_WIRED = 8;

    String CONTEXT_TYPE_AMBIENT = "ambient";
    String CONTEXT_TYPE_DEVICE = "device";
    String CONTEXT_TYPE_USER = "user";
    String CONTEXT_TYPE_VEHICLE = "vehicle";

    String PROP_IS_CONTROLLABLE = "controllable";
    String PROP_IS_REPORTING_ERRORS = "errorsReported";
    String PROP_LATITUDE = "latitude";
    String PROP_LOCATION = "location";
    String PROP_LONGITUDE = "longitude";
    String PROP_MAX_RATE = "maxSamplingRate";
    String PROP_PERMISSION = "permission";
    String PROP_VENDOR = "vendor";
    String PROP_VERSION = "version";

    ChannelInfo[] getChannelInfos();

    int getConnectionType();

    String getContextType();

    String getDescription();

    /** Buffer size a Data object may hold; the specification requires >= 256. */
    int getMaxBufferSize();

    String getModel();

    String[] getPropertyNames();

    Object getProperty(String name);

    /** "acceleration", "angular_velocity", ... (specification table). */
    String getQuantity();

    /** The URL to hand to Connector.open(). */
    String getUrl();

    boolean isAvailable();

    boolean isAvailabilityPushSupported();

    boolean isConditionPushSupported();
}
