package com.siemens.mp.io;

import com.siemens.mp.NotAllowedException;

/**
 * Siemens API io.Connection: an in-process loopback. send() hands the data to
 * the registered listeners; nothing leaves the device. Behaviour follows
 * J2ME-Loader's class (Apache-2.0).
 */
public class Connection extends com.siemens.mp.misc.NativeMem {
    private final String connectTo;
    private static ConnectionListener sListener;
    private ConnectionListener mListener;

    public Connection(String connectTo) {
        this.connectTo = connectTo;
    }

    public ConnectionListener getListener() {
        return mListener;
    }

    public void send(byte[] data) throws NotAllowedException {
        if (sListener != null) {
            sListener.receiveData(data);
        }
        if (mListener != null) {
            mListener.receiveData(data);
        }
    }

    public void setListener(ConnectionListener listener) {
        mListener = listener;
    }

    /* kept under this name for compatibility with Loader's versioning fix */
    public static void setListenerCompat(ConnectionListener listener) {
        sListener = listener;
    }
}
