/*
 * SensorConditionTest.java - host-side test for the JSR-256 condition classes
 * (v04.46). Lives in the javax.microedition.sensor package so it can build a
 * VitaSensorConnection directly (its constructor is package-private).
 *
 * Not shipped: it runs on the build machine with the CLDC class library on the
 * classpath, and checks the behaviours the annotation-verified specification
 * actually states, rather than "it compiled".
 */
package javax.microedition.sensor;

public final class SensorConditionTest {

    private static int pass = 0;
    private static int fail = 0;

    private static void check(String what, boolean ok) {
        if (ok) {
            pass++;
        } else {
            fail++;
            System.out.println("FAIL: " + what);
        }
    }

    private static void checkEq(String what, double got, double want) {
        check(what + " (got " + got + ", want " + want + ")", got == want);
    }

    /* ---- helpers that assert on exceptions ---------------------- */

    private static void npe(String what, Runnable r) {
        try {
            r.run();
            check(what + ": expected NullPointerException", false);
        } catch (NullPointerException e) {
            pass++;
        } catch (Throwable t) {
            check(what + ": got " + t.getClass().getName()
                  + " instead of NullPointerException", false);
        }
    }

    private static void iae(String what, Runnable r) {
        try {
            r.run();
            check(what + ": expected IllegalArgumentException", false);
        } catch (IllegalArgumentException e) {
            pass++;
        } catch (Throwable t) {
            check(what + ": got " + t.getClass().getName()
                  + " instead of IllegalArgumentException", false);
        }
    }

    private static void ise(String what, Runnable r) {
        try {
            r.run();
            check(what + ": expected IllegalStateException", false);
        } catch (IllegalStateException e) {
            pass++;
        } catch (Throwable t) {
            check(what + ": got " + t.getClass().getName()
                  + " instead of IllegalStateException", false);
        }
    }

    private static Runnable run(final Object target, final int which) {
        return new Runnable() {
            public void run() {
                try {
                    dispatch(target, which);
                } catch (RuntimeException e) {
                    throw e;
                }
            }
        };
    }

    /* Minimal "call this method" trampoline: avoids anonymous classes per case
     * (javac 1.3 target, and this file is diagnostics, not production code). */
    private static void dispatch(Object t, int which) {
        if (which == 1) {
            new LimitCondition(1.0, null);
        } else if (which == 2) {
            new LimitCondition(1.0, "nope");
        } else if (which == 3) {
            new RangeCondition(10.0, Condition.OP_GREATER_THAN,
                               5.0, Condition.OP_LESS_THAN);
        } else if (which == 4) {
            new RangeCondition(5.0, Condition.OP_GREATER_THAN,
                               5.0, Condition.OP_LESS_THAN_OR_EQUALS);
        } else if (which == 5) {
            new RangeCondition(0.0, Condition.OP_LESS_THAN,
                               5.0, Condition.OP_LESS_THAN);
        } else if (which == 6) {
            new RangeCondition(0.0, Condition.OP_GREATER_THAN,
                               5.0, Condition.OP_GREATER_THAN);
        } else if (which == 7) {
            new ObjectCondition(null);
        }
    }

    /** Records what conditionMet() received (needs named fields, so not an
     *  anonymous class - a ConditionListener reference cannot expose them). */
    static final class Rec implements ConditionListener {
        int hits = 0;
        Condition cond = null;
        Data data = null;
        SensorConnection sensor = null;

        public void conditionMet(SensorConnection s, Data d, Condition c) {
            hits++;
            cond = c;
            data = d;
            sensor = s;
        }
    }

    public static void main(String[] args) throws Exception {

        /* ---- LimitCondition: all five operators --------------- */
        LimitCondition gt = new LimitCondition(5.0, Condition.OP_GREATER_THAN);
        check("gt 6", gt.isMet(6.0));
        check("gt !5", !gt.isMet(5.0));
        check("gt !object", !gt.isMet(new Object()));
        checkEq("gt getLimit", gt.getLimit(), 5.0);
        check("gt getOperator", "gt".equals(gt.getOperator()));

        check("ge 5", new LimitCondition(5.0, Condition.OP_GREATER_THAN_OR_EQUALS).isMet(5.0));
        check("lt 4.9", new LimitCondition(5.0, Condition.OP_LESS_THAN).isMet(4.9));
        check("lt !5", !new LimitCondition(5.0, Condition.OP_LESS_THAN).isMet(5.0));
        check("le 5", new LimitCondition(5.0, Condition.OP_LESS_THAN_OR_EQUALS).isMet(5.0));
        check("eq 5", new LimitCondition(5.0, Condition.OP_EQUALS).isMet(5.0));
        check("eq !5.1", !new LimitCondition(5.0, Condition.OP_EQUALS).isMet(5.1));

        /* the operator must be compared by CONTENT: an application may pass
         * its own String instance holding "gt" */
        LimitCondition copy = new LimitCondition(5.0, new String("gt"));
        check("operator compared with equals()", copy.isMet(6.0));

        npe("LimitCondition(null operator)", run(gt, 1));
        iae("LimitCondition(bad operator)", run(gt, 2));

        /* ---- RangeCondition: four range shapes ---------------- */
        RangeCondition open = new RangeCondition(0.0, Condition.OP_GREATER_THAN,
                                                 10.0, Condition.OP_LESS_THAN);
        check("open 5", open.isMet(5.0));
        check("open !0", !open.isMet(0.0));
        check("open !10", !open.isMet(10.0));
        check("open !object", !open.isMet(new Object()));
        checkEq("open getLowerLimit", open.getLowerLimit(), 0.0);
        checkEq("open getUpperLimit", open.getUpperLimit(), 10.0);
        check("open getLowerOp", "gt".equals(open.getLowerOp()));
        check("open getUpperOp", "lt".equals(open.getUpperOp()));

        RangeCondition closed = new RangeCondition(0.0, Condition.OP_GREATER_THAN_OR_EQUALS,
                                                    10.0, Condition.OP_LESS_THAN_OR_EQUALS);
        check("closed 0", closed.isMet(0.0));
        check("closed 10", closed.isMet(10.0));

        RangeCondition halfLo = new RangeCondition(0.0, Condition.OP_GREATER_THAN_OR_EQUALS,
                                                    10.0, Condition.OP_LESS_THAN);
        check("halfLo 0", halfLo.isMet(0.0));
        check("halfLo !10", !halfLo.isMet(10.0));

        RangeCondition halfHi = new RangeCondition(0.0, Condition.OP_GREATER_THAN,
                                                    10.0, Condition.OP_LESS_THAN_OR_EQUALS);
        check("halfHi !0", !halfHi.isMet(0.0));
        check("halfHi 10", halfHi.isMet(10.0));

        iae("RangeCondition lower>upper", run(open, 3));
        iae("RangeCondition empty (equal limits, gt..le)", run(open, 4));
        iae("RangeCondition bad lowerOp", run(open, 5));
        iae("RangeCondition bad upperOp", run(open, 6));

        /* equal limits ARE valid when both ends are inclusive */
        RangeCondition point = new RangeCondition(5.0, Condition.OP_GREATER_THAN_OR_EQUALS,
                                                   5.0, Condition.OP_LESS_THAN_OR_EQUALS);
        check("point range 5", point.isMet(5.0));
        check("point range !4", !point.isMet(4.0));

        /* ---- ObjectCondition ---------------------------------- */
        ObjectCondition oc = new ObjectCondition("hello");
        check("object equals", oc.isMet((Object) "hello"));
        check("object !equals", !oc.isMet((Object) "world"));
        check("object !double", !oc.isMet(1.0));
        check("object getLimit", "hello".equals(oc.getLimit()));
        npe("ObjectCondition(null)", run(oc, 7));

        /* ---- Channel + connection wiring ---------------------- */
        VitaSensorInfo si = new VitaSensorInfo("acceleration",
                SensorInfo.CONTEXT_TYPE_DEVICE,
                new String[] { "axis_x", "axis_y", "axis_z" },
                VitaSensorInfo.KIND_ACCEL);
        final VitaSensorConnection conn = new VitaSensorConnection(si);
        final ChannelInfo[] cis = si.getChannelInfos();
        final Channel chx = conn.getChannel(cis[0]);
        check("channel keeps the ChannelInfo instance", chx.getChannelInfo() == cis[0]);
        check("same Channel on a repeat call", conn.getChannel(cis[0]) == chx);

        final Channel fchx = chx;
        final Rec rec = new Rec();
        final Condition hit = new LimitCondition(-1000.0, Condition.OP_GREATER_THAN);

        chx.addCondition(rec, hit);
        check("condition registered", chx.getConditions(rec).length == 1);
        chx.addCondition(rec, hit);
        check("duplicate (listener, condition) pair ignored",
              chx.getConditions(rec).length == 1);

        /* getData() must test every value; -1000 is below any acceleration, so
         * the FIRST value meets it and the condition is removed at once. */
        conn.getData(3);
        checkEq("conditionMet called once", (double) rec.hits, 1.0);
        check("notified condition is the registered instance", rec.cond == hit);
        check("notified connection is this one", rec.sensor == conn);
        checkEq("notified Data has one value",
                (double) rec.data.getDoubleValues().length, 1.0);
        check("notified Data validity is true", rec.data.isValid(0));
        check("condition auto-removed from the channel",
              chx.getConditions(rec).length == 0);

        conn.getData(1);
        checkEq("no notification after removal", (double) rec.hits, 1.0);

        /* removeCondition keys on INSTANCE identity */
        Channel chy = conn.getChannel(cis[1]);
        chy.addCondition(rec, new LimitCondition(-1000.0, Condition.OP_GREATER_THAN));
        chy.removeCondition(rec, new LimitCondition(-1000.0, Condition.OP_GREATER_THAN));
        check("removeCondition ignores a different instance",
              chy.getConditions(rec).length == 1);
        chy.removeConditionListener(rec);
        check("removeConditionListener clears them all",
              chy.getConditions(rec).length == 0);

        chx.addCondition(rec, hit);
        chx.removeCondition(rec, hit);
        check("removeCondition with the same instance",
              chx.getConditions(rec).length == 0);

        /* argument and type checking */
        final Rec rec2 = new Rec();
        iae("ObjectCondition on a numeric channel", new Runnable() {
            public void run() { fchx.addCondition(rec2, new ObjectCondition("x")); }
        });
        npe("addCondition(null listener)", new Runnable() {
            public void run() {
                fchx.addCondition(null, new LimitCondition(1.0, Condition.OP_EQUALS));
            }
        });
        npe("addCondition(null condition)", new Runnable() {
            public void run() { fchx.addCondition(rec2, null); }
        });
        iae("getChannel with a foreign ChannelInfo", new Runnable() {
            public void run() {
                fchx.getChannelInfo();
                conn.getChannel(new VitaChannelInfo("bogus", VitaSensorInfo.KIND_ACCEL));
            }
        });

        /* getChannelUrl lists unique conditions by the channel's name */
        chx.addCondition(rec2, new LimitCondition(5.0, Condition.OP_GREATER_THAN));
        String url = chx.getChannelUrl();
        check("channel url starts with the channel name: " + url,
              url.startsWith("axis_x="));
        check("channel url carries the operator: " + url, url.indexOf("gt:") > 0);
        chx.removeAllConditions();
        check("removeAllConditions", chx.getConditions(rec2).length == 0);

        /* a closed connection rejects condition changes */
        conn.close();
        ise("addCondition after close", new Runnable() {
            public void run() {
                fchx.addCondition(rec2, new LimitCondition(1.0, Condition.OP_EQUALS));
            }
        });
        ise("removeAllConditions after close", new Runnable() {
            public void run() { fchx.removeAllConditions(); }
        });

        System.out.println("PASS=" + pass + " FAIL=" + fail);
        if (fail != 0) {
            throw new RuntimeException(fail + " check(s) failed");
        }
    }
}
