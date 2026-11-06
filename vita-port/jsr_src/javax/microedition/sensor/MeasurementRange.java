/*
 * MeasurementRange.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/** One measurement range of a channel: limits plus resolution. Immutable. */
public class MeasurementRange {

    private final double smallest;
    private final double largest;
    private final double resolution;

    public MeasurementRange(double smallest, double largest,
                            double resolution) {
        if (smallest > largest) {
            throw new IllegalArgumentException("smallest > largest");
        }
        if (resolution < 0.0) {
            throw new IllegalArgumentException("negative resolution");
        }
        this.smallest = smallest;
        this.largest = largest;
        this.resolution = resolution;
    }

    public double getSmallestValue() {
        return smallest;
    }

    public double getLargestValue() {
        return largest;
    }

    public double getResolution() {
        return resolution;
    }
}
