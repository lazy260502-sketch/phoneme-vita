package com.sun.m3g;

/**
 * Diagnostics sink for the M3G layer.
 *
 * The launcher provides crumb_printf through a weak symbol; this class only
 * needs somewhere to put short lines, so it writes to stderr when there is
 * nothing better. Kept separate so the loader does not depend on the
 * launcher.
 */
public final class M3gTrace {

    private M3gTrace() {
    }

    public static void log(String msg) {
        System.err.println("[m3g] " + msg);
    }

    /* v03.28: make a swallowed exception visible.
     *
     * The title that fails in log133 wraps its whole M3G frame in
     * `catch (Exception e) { canvas.printException(...) }`, so an exception
     * thrown by this layer is drawn on the 2D HUD and never reaches any log -
     * the device log showed models loading, a working backend and zero
     * exceptions, while the screen stayed empty. Every `throw new XxxException`
     * in this package is now routed through fail(), which records the message
     * once and returns the same exception so behaviour is unchanged.
     *
     * Bounded: a per-frame failure could otherwise emit thousands of lines
     * (log133 ran for thousands of frames). After FAIL_MAX reports only the
     * total count is tracked. */
    private static int failCount = 0;
    private static final int FAIL_MAX = 40;

    public static RuntimeException fail(RuntimeException e) {
        failCount++;
        if (failCount <= FAIL_MAX) {
            log("threw " + e);
        } else if (failCount == FAIL_MAX + 1) {
            log("threw ... (further occurrences suppressed)");
        }
        return e;
    }

    /** Count of exceptions raised by this package so far. */
    public static int failCount() {
        return failCount;
    }
}
