/*
 * SensorListener.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/** Availability notifications (the port's sensors never go away). */
public interface SensorListener {

    void sensorAvailable(SensorInfo info);

    void sensorUnavailable(SensorInfo info);
}
