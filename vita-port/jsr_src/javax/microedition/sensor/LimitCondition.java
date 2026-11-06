/*
 * LimitCondition.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * Specification points implemented literally:
 *   - constructor: NPE when operator is null, IAE when it is not one of the
 *     five Condition operators;
 *   - the object is immutable (final fields);
 *   - isMet(Object) returns false ALWAYS - the specification says numeric
 *     conditions only answer isMet(double), ObjectCondition handles objects.
 */
package javax.microedition.sensor;

/** A single-threshold condition on numeric data. */
public final class LimitCondition implements Condition {

    private final double limit;
    private final String operator;

    public LimitCondition(double limit, String operator) {
        if (operator == null) {
            throw new NullPointerException("operator");
        }
        if (!validOperator(operator)) {
            throw new IllegalArgumentException("unknown operator: " + operator);
        }
        this.limit = limit;
        this.operator = operator;
    }

    private static boolean validOperator(String op) {
        return op.equals(Condition.OP_EQUALS)
            || op.equals(Condition.OP_GREATER_THAN)
            || op.equals(Condition.OP_GREATER_THAN_OR_EQUALS)
            || op.equals(Condition.OP_LESS_THAN)
            || op.equals(Condition.OP_LESS_THAN_OR_EQUALS);
    }

    public final double getLimit() {
        return limit;
    }

    public final String getOperator() {
        return operator;
    }

    public boolean isMet(double value) {
        if (operator.equals(Condition.OP_EQUALS)) {
            return value == limit;
        }
        if (operator.equals(Condition.OP_GREATER_THAN)) {
            return value > limit;
        }
        if (operator.equals(Condition.OP_GREATER_THAN_OR_EQUALS)) {
            return value >= limit;
        }
        if (operator.equals(Condition.OP_LESS_THAN)) {
            return value < limit;
        }
        /* constructor guarantees this is OP_LESS_THAN_OR_EQUALS */
        return value <= limit;
    }

    /** Always false: numeric conditions do not test objects. */
    public boolean isMet(Object value) {
        return false;
    }
}
