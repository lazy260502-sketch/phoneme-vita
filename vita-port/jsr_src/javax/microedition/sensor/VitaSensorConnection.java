/*
 * VitaSensorConnection.java - the open sensor (see VitaSensor.java for the
 * data-source note; VitaData.java holds the Data objects).
 *
 * Two retrieval modes:
 *   synchronous  - getData() samples on the caller's thread, no thread at all;
 *   asynchronous - setDataListener() starts ONE low-priority pump thread that
 *                  sleeps for the buffering period (or bufferSize * 20 ms) and
 *                  then calls dataReceived(). close()/removeDataListener()
 *                  stop it, and the loop also exits on any throwable so a
 *                  misbehaving listener cannot keep the VM awake.
 */
package javax.microedition.sensor;

import java.io.IOException;

final class VitaSensorConnection implements SensorConnection {

    private static final int SAMPLE_MS = 20;    /* 50 Hz */

    final VitaSensorInfo info;
    private volatile int state = STATE_OPENED;

    /* v04.46: one VitaChannel per channel, created on first use and shared
     * afterwards so that conditions registered through getChannel() survive
     * repeated calls (the specification's instance-equality rule depends on
     * the Channel being stable too). */
    private VitaChannel[] chans;

    private DataListener listener;
    private Thread pump;
    private int pumpBuf = 1;

    VitaSensorConnection(VitaSensorInfo info) {
        this.info = info;
    }

    /* ---- synchronous ------------------------------------------- */

    public Data[] getData(int bufferSize) throws IOException {
        return getData(bufferSize, -1, false, false, false);
    }

    public Data[] getData(int bufferSize, long bufferingPeriod,
                          boolean isTimestampIncluded,
                          boolean isUncertaintyIncluded,
                          boolean isValidityIncluded) throws IOException {
        if (state == STATE_CLOSED) {
            throw new IOException("connection closed");
        }
        if (state == STATE_LISTENING) {
            throw new IllegalStateException("listening: use the DataListener");
        }
        int n = bufferSize;
        if (n < 1) {
            if (bufferingPeriod < 1) {
                throw new IllegalArgumentException("need bufferSize or period");
            }
            n = (int) (bufferingPeriod / SAMPLE_MS);
            if (n < 1) {
                n = 1;
            }
        }
        if (n > VitaSensor.MAX_BUFFER) {
            throw new IllegalArgumentException("bufferSize too large");
        }
        return collect(n, isTimestampIncluded, isUncertaintyIncluded,
                       isValidityIncluded);
    }

    private Data[] collect(int n, boolean withTs, boolean withUnc,
                           boolean withValid) {
        long now = System.currentTimeMillis();
        ChannelInfo[] ch = info.getChannelInfos();
        Data[] out = new Data[ch.length];
        for (int c = 0; c < ch.length; c++) {
            double[] v = new double[n];
            long[] ts = withTs ? new long[n] : null;
            float[] unc = withUnc ? new float[n] : null;
            boolean[] ok = withValid ? new boolean[n] : null;
            for (int i = 0; i < n; i++) {
                long when = now - (long) (n - 1 - i) * SAMPLE_MS;
                v[i] = VitaSensor.value(info.kind, c, when);
                /* v04.46: condition monitoring sees every produced value.
                 * Both retrieval modes come through collect(), so a game gets
                 * the same conditionMet() notifications either way. */
                channelAt(c).checkValue(v[i], when);
                if (ts != null) {
                    ts[i] = when;
                }
                if (unc != null) {
                    unc[i] = 0.01f;
                }
                if (ok != null) {
                    ok[i] = true;
                }
            }
            out[c] = new VitaData(ch[c], v, ts, unc, ok);
        }
        return out;
    }

    /* ---- asynchronous ------------------------------------------ */

    public void setDataListener(DataListener l, int bufferSize)
            throws IOException {
        setDataListener(l, bufferSize, -1, false, false, false);
    }

    public void setDataListener(DataListener l, int bufferSize,
                                long bufferingPeriod,
                                boolean isTimestampIncluded,
                                boolean isUncertaintyIncluded,
                                boolean isValidityIncluded) throws IOException {
        if (l == null) {
            throw new NullPointerException("listener");
        }
        if (state == STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        if (bufferSize > VitaSensor.MAX_BUFFER) {
            throw new IllegalArgumentException("bufferSize too large");
        }
        stopPump();
        listener = l;
        state = STATE_LISTENING;
        startPump(bufferSize, bufferingPeriod);
    }

    private void startPump(int bufSize, long period) {
        pumpBuf = (bufSize > 0) ? bufSize : 1;
        final long sleep = (period > 0) ? period : (long) pumpBuf * SAMPLE_MS;
        final DataListener l = listener;
        pump = new Thread(new Runnable() {
            public void run() {
                while (state == STATE_LISTENING) {
                    try {
                        Thread.sleep(sleep);
                    } catch (InterruptedException e) {
                        return;
                    }
                    if (state != STATE_LISTENING) {
                        return;
                    }
                    try {
                        l.dataReceived(VitaSensorConnection.this,
                                collect(pumpBuf, false, false, false), false);
                    } catch (Throwable t) {
                        return;     /* a throwing listener must not spin */
                    }
                }
            }
        });
        pump.start();
    }

    private void stopPump() {
        if (pump != null) {
            Thread t = pump;
            pump = null;
            t.interrupt();
        }
    }

    public void removeDataListener() {
        if (state == STATE_CLOSED) {
            throw new IllegalStateException("connection closed");
        }
        stopPump();
        listener = null;
        state = STATE_OPENED;
    }

    /* ---- Connection -------------------------------------------- */

    public SensorInfo getSensorInfo() {
        return info;
    }

    /** Lazily builds (and then reuses) the Channel array. */
    private synchronized VitaChannel channelAt(int idx) {
        if (chans == null) {
            ChannelInfo[] ci = info.getChannelInfos();
            VitaChannel[] a = new VitaChannel[ci.length];
            for (int i = 0; i < ci.length; i++) {
                a[i] = new VitaChannel(this, ci[i], i);
            }
            chans = a;
        }
        return chans[idx];
    }

    public Channel getChannel(ChannelInfo channelInfo) {
        if (channelInfo == null) {
            throw new NullPointerException("channelInfo");
        }
        ChannelInfo[] ci = info.getChannelInfos();
        for (int i = 0; i < ci.length; i++) {
            /* Instance equality, as the specification demands. */
            if (ci[i] == channelInfo) {
                return channelAt(i);
            }
        }
        throw new IllegalArgumentException(
                "not a ChannelInfo of this sensor (use getChannelInfos())");
    }

    public int getState() {
        return state;
    }

    public void close() {
        stopPump();
        listener = null;
        state = STATE_CLOSED;
    }
}
