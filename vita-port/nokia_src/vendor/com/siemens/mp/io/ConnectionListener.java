package com.siemens.mp.io;

/** Siemens API io.ConnectionListener (J2ME-Loader list, Apache-2.0). */
public interface ConnectionListener {
    void receiveData(byte[] data);
}
