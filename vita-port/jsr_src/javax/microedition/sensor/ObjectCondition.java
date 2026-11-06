/*
 * ObjectCondition.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * Equality is tested with the LIMIT's own equals(), which is what the
 * specification says - so a String limit compares by content and a byte[]
 * limit compares by identity unless the application wraps it in something with
 * a meaningful equals(). isMet(double) returns false always.
 */
package javax.microedition.sensor;

/** Equality condition for channels whose data type is ChannelInfo.TYPE_OBJECT. */
public final class ObjectCondition implements Condition {

    private final Object limit;

    public ObjectCondition(Object limit) {
        if (limit == null) {
            throw new NullPointerException("limit");
        }
        this.limit = limit;
    }

    public final Object getLimit() {
        return limit;
    }

    /** Always false: object conditions do not test numeric values. */
    public boolean isMet(double value) {
        return false;
    }

    public boolean isMet(Object value) {
        return limit.equals(value);
    }
}
