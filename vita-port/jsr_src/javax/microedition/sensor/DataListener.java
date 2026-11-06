/*
 * DataListener.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/** Implemented by a MIDlet that wants asynchronous sensor data. */
public interface DataListener {

    void dataReceived(SensorConnection sensor, Data[] data,
                      boolean isDataLost);
}
