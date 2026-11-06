/*
 * ConditionListener.java - JSR-256 (see ChannelInfo.java for the scope note).
 */
package javax.microedition.sensor;

/**
 * Receives one notification per condition that a measured value meets. The
 * implementation MUST call this sequentially and MUST remove the condition
 * from the channel before calling, so one value produces at most one
 * notification per condition and the application is not flooded.
 */
public interface ConditionListener {

    void conditionMet(SensorConnection sensor, Data data, Condition condition);
}
