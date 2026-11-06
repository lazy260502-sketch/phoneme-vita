/*
 * RangeCondition.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * The specification lists exactly three invalid-argument cases, all enforced
 * here in the constructor:
 *   1. lowerLimit > upperLimit;
 *   2. lowerLimit == upperLimit unless BOTH ends are inclusive
 *      (lowerOp == OP_GREATER_THAN_OR_EQUALS and upperOp == OP_LESS_THAN_OR_EQUALS);
 *   3. lowerOp not in {OP_GREATER_THAN, OP_GREATER_THAN_OR_EQUALS} or upperOp
 *      not in {OP_LESS_THAN, OP_LESS_THAN_OR_EQUALS}.
 * Note that a null operator is an IllegalArgumentException here, not an NPE:
 * the specification only documents IAE for this constructor.
 */
package javax.microedition.sensor;

/** A two-threshold (open / closed / half-open) range on numeric data. */
public final class RangeCondition implements Condition {

    private final double lowerLimit;
    private final String lowerOp;
    private final double upperLimit;
    private final String upperOp;

    public RangeCondition(double lowerLimit, String lowerOp,
                          double upperLimit, String upperOp) {
        if (!(Condition.OP_GREATER_THAN.equals(lowerOp)
                || Condition.OP_GREATER_THAN_OR_EQUALS.equals(lowerOp))) {
            throw new IllegalArgumentException("lowerOp: " + lowerOp);
        }
        if (!(Condition.OP_LESS_THAN.equals(upperOp)
                || Condition.OP_LESS_THAN_OR_EQUALS.equals(upperOp))) {
            throw new IllegalArgumentException("upperOp: " + upperOp);
        }
        if (lowerLimit > upperLimit) {
            throw new IllegalArgumentException("lowerLimit > upperLimit");
        }
        if (lowerLimit == upperLimit
                && !(Condition.OP_GREATER_THAN_OR_EQUALS.equals(lowerOp)
                     && Condition.OP_LESS_THAN_OR_EQUALS.equals(upperOp))) {
            throw new IllegalArgumentException("empty range");
        }
        this.lowerLimit = lowerLimit;
        this.lowerOp = lowerOp;
        this.upperLimit = upperLimit;
        this.upperOp = upperOp;
    }

    public final double getLowerLimit() {
        return lowerLimit;
    }

    public final String getLowerOp() {
        return lowerOp;
    }

    public final double getUpperLimit() {
        return upperLimit;
    }

    public final String getUpperOp() {
        return upperOp;
    }

    public boolean isMet(double value) {
        boolean lo = Condition.OP_GREATER_THAN_OR_EQUALS.equals(lowerOp)
                ? (value >= lowerLimit) : (value > lowerLimit);
        boolean hi = Condition.OP_LESS_THAN_OR_EQUALS.equals(upperOp)
                ? (value <= upperLimit) : (value < upperLimit);
        return lo && hi;
    }

    /** Always false: numeric conditions do not test objects. */
    public boolean isMet(Object value) {
        return false;
    }
}
