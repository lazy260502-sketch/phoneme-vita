/*
 * Data.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/** Values of one channel, optionally with timestamp/uncertainty/validity. */
public interface Data {

    ChannelInfo getChannelInfo();

    double[] getDoubleValues();

    int[] getIntValues();

    Object[] getObjectValues();

    long getTimestamp(int index);

    float getUncertainty(int index);

    boolean isValid(int index);
}
