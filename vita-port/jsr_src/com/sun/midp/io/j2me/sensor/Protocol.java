/*
 * Protocol.java - the "sensor:" scheme for javax.microedition.io.Connector.
 *
 * JSR-256 has no open method of its own: a MIDlet opens a sensor with
 *     Connector.open("sensor://acceleration;contextType=device;model=vita")
 * and the MIDP Connector resolves the scheme by instantiating
 * com.sun.midp.io.j2me.<scheme>.Protocol.
 *
 * That lookup is the reason this class sits under com.sun - it is the ONLY
 * com.sun package this port ships, whitelisted as SUN_ALLOW in
 * vita-port/build_nokia_stubs.sh. Everything else under com.sun belongs to
 * MIDP and is ROMized.
 */
package com.sun.midp.io.j2me.sensor;

import java.io.IOException;

import javax.microedition.io.Connection;
import javax.microedition.sensor.SensorManager;

import com.sun.cldc.io.ConnectionBaseInterface;

public class Protocol implements ConnectionBaseInterface {

    public Protocol() {
    }

    public Connection openPrim(String name, int mode, boolean timeouts)
            throws IOException {
        /* `name` arrives as the URL with "sensor:" stripped, e.g.
         * "//acceleration;contextType=device;model=vita". */
        return SensorManager.openInternal(name);
    }
}
