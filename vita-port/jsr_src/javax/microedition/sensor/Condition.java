/*
 * Condition.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * v04.46: previously absent. This port had no condition monitoring at all, so
 * a game that called Channel.addCondition() hit NoClassDefFoundError on the
 * Condition type. Signatures and the five operator strings were taken from the
 * specification's javadoc (nikita36078.github.io JSR-256 mirror) and its
 * constant-values page; OP_* really are the two-letter strings below ("eq",
 * "gt", "ge", "lt", "le") because the same strings appear in the sensor URL
 * scheme, and they are compared with equals(), not ==, since the application
 * may pass its own instances.
 */
package javax.microedition.sensor;

/**
 * Test applied to every measured value of a channel. The specification ships
 * three concrete implementations ({@link LimitCondition},
 * {@link RangeCondition}, {@link ObjectCondition}); an application may add its
 * own by implementing this interface.
 */
public interface Condition {

    String OP_EQUALS = "eq";
    String OP_GREATER_THAN = "gt";
    String OP_GREATER_THAN_OR_EQUALS = "ge";
    String OP_LESS_THAN = "lt";
    String OP_LESS_THAN_OR_EQUALS = "le";

    /** Tests a numeric measurement. */
    boolean isMet(double doubleValue);

    /**
     * Tests an object measurement. The numeric conditions return false here,
     * and ObjectCondition returns false for double - see each class.
     */
    boolean isMet(Object value);
}
