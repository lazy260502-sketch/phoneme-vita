/*
 * VitaChannel.java - JSR-256 condition monitoring (v04.46).
 *
 * One instance per channel of an open connection, created lazily by
 * VitaSensorConnection.getChannel(). Conditions are kept in two parallel
 * Vectors (listener[i] owns condition[i]) because the specification needs
 * INSTANCE identity for removeCondition/getConditions, and Vector keeps
 * registration order - which is the order notifications must follow.
 *
 * Notification contract from the specification, implemented literally:
 *   - every produced value is tested against every registered condition;
 *   - a condition that is met is REMOVED before the listener is called, so a
 *     stream of values above a threshold cannot flood the application;
 *   - the Data handed to conditionMet carries value + timestamp + uncertainty
 *     and validity, and validity is always true;
 *   - listeners are called outside the lock (a listener may call
 *     addCondition() from inside conditionMet()).
 */
package javax.microedition.sensor;

import java.util.Vector;

final class VitaChannel implements Channel {

    private final VitaSensorConnection conn;
    private final ChannelInfo ci;
    private final int index;

    private final Vector listeners = new Vector();
    private final Vector conditions = new Vector();

    VitaChannel(VitaSensorConnection conn, ChannelInfo ci, int index) {
        this.conn = conn;
        this.ci = ci;
        this.index = index;
    }

    /* ---- condition registration -------------------------------- */

    public synchronized void addCondition(ConditionListener listener,
                                         Condition condition) {
        if (listener == null) {
            throw new NullPointerException("listener");
        }
        if (condition == null) {
            /* No PushRegistry in this port, so the specification's "null means
             * pending push condition" case cannot apply: reject it. */
            throw new NullPointerException("condition");
        }
        if (conn.getState() == SensorConnection.STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        checkType(condition);
        for (int i = 0; i < conditions.size(); i++) {
            if (listeners.elementAt(i) == listener
                    && conditions.elementAt(i) == condition) {
                return;     /* same pair: ignored silently, per specification */
            }
        }
        listeners.addElement(listener);
        conditions.addElement(condition);
    }

    private void checkType(Condition c) {
        int t = ci.getDataType();
        if (t == ChannelInfo.TYPE_OBJECT) {
            if (c instanceof LimitCondition || c instanceof RangeCondition) {
                throw new IllegalArgumentException(
                        "numeric condition on an object channel");
            }
        } else if (c instanceof ObjectCondition) {
            throw new IllegalArgumentException(
                    "ObjectCondition on a numeric channel");
        }
    }

    public synchronized Condition[] getConditions(ConditionListener listener) {
        if (listener == null) {
            throw new NullPointerException("listener");
        }
        int n = 0;
        int i;
        for (i = 0; i < listeners.size(); i++) {
            if (listeners.elementAt(i) == listener) {
                n++;
            }
        }
        Condition[] out = new Condition[n];
        n = 0;
        for (i = 0; i < listeners.size(); i++) {
            if (listeners.elementAt(i) == listener) {
                out[n++] = (Condition) conditions.elementAt(i);
            }
        }
        return out;
    }

    public synchronized void removeCondition(ConditionListener listener,
                                             Condition condition) {
        if (listener == null || condition == null) {
            throw new NullPointerException("listener/condition");
        }
        if (conn.getState() == SensorConnection.STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        for (int i = 0; i < conditions.size(); i++) {
            if (listeners.elementAt(i) == listener
                    && conditions.elementAt(i) == condition) {
                listeners.removeElementAt(i);
                conditions.removeElementAt(i);
                return;
            }
        }
        /* not registered: silently ignored, per specification */
    }

    public synchronized void removeConditionListener(
            ConditionListener listener) {
        if (listener == null) {
            throw new NullPointerException("listener");
        }
        if (conn.getState() == SensorConnection.STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        for (int i = listeners.size() - 1; i >= 0; i--) {
            if (listeners.elementAt(i) == listener) {
                listeners.removeElementAt(i);
                conditions.removeElementAt(i);
            }
        }
    }

    public synchronized void removeAllConditions() {
        if (conn.getState() == SensorConnection.STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        listeners.removeAllElements();
        conditions.removeAllElements();
    }

    /* ---- identification ---------------------------------------- */

    public ChannelInfo getChannelInfo() {
        return ci;
    }

    public synchronized String getChannelUrl() {
        StringBuffer sb = new StringBuffer(ci.getName());
        Vector seen = new Vector();
        boolean first = true;
        for (int i = 0; i < conditions.size(); i++) {
            Condition c = (Condition) conditions.elementAt(i);
            if (containsIdentical(seen, c)) {
                continue;
            }
            seen.addElement(c);
            sb.append(first ? '=' : '&');
            first = false;
            describe(sb, c);
        }
        return sb.toString();
    }

    private static boolean containsIdentical(Vector v, Object o) {
        for (int i = 0; i < v.size(); i++) {
            if (v.elementAt(i) == o) {
                return true;
            }
        }
        return false;
    }

    private static void describe(StringBuffer sb, Condition c) {
        if (c instanceof LimitCondition) {
            LimitCondition lc = (LimitCondition) c;
            sb.append(lc.getOperator()).append(':').append(lc.getLimit());
        } else if (c instanceof RangeCondition) {
            RangeCondition rc = (RangeCondition) c;
            sb.append(rc.getLowerOp()).append(':').append(rc.getLowerLimit())
              .append(',').append(rc.getUpperOp()).append(':')
              .append(rc.getUpperLimit());
        } else if (c instanceof ObjectCondition) {
            sb.append(Condition.OP_EQUALS).append(':')
              .append(((ObjectCondition) c).getLimit());
        } else {
            sb.append("custom");
        }
    }

    int getIndex() {
        return index;
    }

    /* ---- the actual monitoring --------------------------------- */

    /**
     * Tests one measured value against every condition and notifies the
     * listeners whose condition was met. Called for every value the connection
     * produces (synchronous and asynchronous alike), so an application gets
     * the same notifications whichever mode it uses.
     */
    void checkValue(double v, long ts) {
        Vector call = null;
        synchronized (this) {
            if (conditions.size() == 0) {
                return;
            }
            for (int i = 0; i < conditions.size(); ) {
                Condition c = (Condition) conditions.elementAt(i);
                if (c.isMet(v)) {
                    if (call == null) {
                        call = new Vector();
                    }
                    call.addElement(listeners.elementAt(i));
                    call.addElement(c);
                    listeners.removeElementAt(i);
                    conditions.removeElementAt(i);
                    /* do not advance: the next condition moved into slot i */
                } else {
                    i++;
                }
            }
        }
        if (call == null) {
            return;
        }
        Data d = new VitaData(ci, new double[] { v }, new long[] { ts },
                              new float[] { 0.01f }, new boolean[] { true });
        for (int i = 0; i < call.size(); i += 2) {
            ConditionListener l = (ConditionListener) call.elementAt(i);
            Condition c = (Condition) call.elementAt(i + 1);
            try {
                l.conditionMet(conn, d, c);
            } catch (Throwable t) {
                /* A throwing listener must not kill the pump thread. */
            }
        }
    }
}