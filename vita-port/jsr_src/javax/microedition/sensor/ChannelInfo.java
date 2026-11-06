/*
 * ChannelInfo.java - JSR-256 (Mobile Sensor API) for the PS Vita port.
 *
 * Scope note: this port implements the parts of JSR-256 a game actually uses -
 * find sensors, open one, read data - driven by a SYNTHETIC source (a slow
 * tilt), not by the console's six-axis unit yet. See VitaSensor.java.
 */
package javax.microedition.sensor;

/** Data properties of one channel of a sensor. */
public interface ChannelInfo {

    int TYPE_DOUBLE = 1;
    int TYPE_INT = 2;
    int TYPE_OBJECT = 4;

    /** Accuracy as a percentage of the reading, or -1 when unknown. */
    float getAccuracy();

    int getDataType();

    MeasurementRange[] getMeasurementRanges();

    /** "axis_x", "axis_y", "axis_z", ... (see the specification's table). */
    String getName();

    /** Power-of-ten scaling factor applied to the values; 0 = none. */
    int getScale();

    Unit getUnit();
}
