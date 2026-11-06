/*
 * Channel.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * Conditions live on the CHANNEL, not on the connection: the specification's
 * ConditionListener javadoc says "add Condition objects to Channel objects",
 * and Channel is where add/remove/getConditions are defined. This interface was
 * missing from the port until v04.46, which is why condition monitoring could
 * not work at all.
 */
package javax.microedition.sensor;

/**
 * One measured quantity of a sensor (an axis, for instance). Holds the
 * conditions that are checked against every value this channel produces.
 */
public interface Channel {

    /**
     * Registers a condition to monitor.
     *
     * @throws NullPointerException  if listener is null, or if condition is
     *         null (this port has no push registry, so the "null means pending
     *         push condition" special case never applies)
     * @throws IllegalArgumentException when the condition type does not match
     *         the channel data type
     * @throws IllegalStateException when the connection is closed
     */
    void addCondition(ConditionListener listener, Condition condition);

    ChannelInfo getChannelInfo();

    /**
     * Identifier plus the unique conditions set on this channel, used to build
     * a push sensor URL.
     *
     * NOTE: the exact grammar of the specification's &lt;channel&gt; part could
     * not be read (the javadoc page truncates before it), so this method emits
     * "name=op:limit" joined with '&amp;', limits in basic units. Nothing in
     * this port uses the result - there is no PushRegistry - so the string is
     * informational only.
     */
    String getChannelUrl();

    /** Conditions registered for this listener; zero-length when none. */
    Condition[] getConditions(ConditionListener listener);

    void removeAllConditions();

    void removeCondition(ConditionListener listener, Condition condition);

    void removeConditionListener(ConditionListener listener);
}
