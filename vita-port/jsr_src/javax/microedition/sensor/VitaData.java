/*
 * VitaData.java - one channel's values (see VitaSensor.java for the scope note).
 *
 * The metadata arrays are null unless the caller asked for them, which is what
 * the "IllegalStateException if not requested" contract is about.
 */
package javax.microedition.sensor;

final class VitaData implements Data {

    private final ChannelInfo ci;
    private final double[] values;
    private final long[] ts;
    private final float[] unc;
    private final boolean[] valid;

    VitaData(ChannelInfo ci, double[] values, long[] ts, float[] unc,
             boolean[] valid) {
        this.ci = ci;
        this.values = values;
        this.ts = ts;
        this.unc = unc;
        this.valid = valid;
    }

    public ChannelInfo getChannelInfo() {
        return ci;
    }

    public double[] getDoubleValues() {
        return values;
    }

    public int[] getIntValues() {
        throw new IllegalStateException("channel is not TYPE_INT");
    }

    public Object[] getObjectValues() {
        throw new IllegalStateException("channel is not TYPE_OBJECT");
    }

    public long getTimestamp(int index) {
        if (ts == null) {
            throw new IllegalStateException("timestamp not requested");
        }
        return ts[index];
    }

    public float getUncertainty(int index) {
        if (unc == null) {
            throw new IllegalStateException("uncertainty not requested");
        }
        return unc[index];
    }

    public boolean isValid(int index) {
        if (valid == null) {
            throw new IllegalStateException("validity not requested");
        }
        return valid[index];
    }
}
