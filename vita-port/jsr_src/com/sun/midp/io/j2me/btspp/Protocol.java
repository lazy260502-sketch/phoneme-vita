/*
 * Protocol.java - the "btspp:" scheme for javax.microedition.io.Connector.
 *
 * JSR-82 opens connections through Connector.open("btspp://..."), so the port
 * needs a protocol handler at
 *     <protocolpath>.<platform>.<scheme>.Protocol
 * =  com.sun.midp.io.j2me.btspp.Protocol   (see vitaport system.config).
 *
 * URL forms:
 *   btspp://localhost:<uuid>;name=...   -> StreamConnectionNotifier
 *   btspp://<12-hex-address>:<channel>  -> StreamConnection (loopback)
 *
 * Both ends sit on one in-process byte bus (see VitaBtConnection.java), so a
 * write is readable from this connection and from any other btspp connection
 * on the same console. There is no radio involved.
 */
package com.sun.midp.io.j2me.btspp;

import java.io.IOException;

import javax.microedition.io.Connection;

import com.sun.cldc.io.ConnectionBaseInterface;

public class Protocol implements ConnectionBaseInterface {

    public Protocol() {
    }

    public Connection openPrim(String name, int mode, boolean timeouts)
            throws IOException {
        if (name == null) {
            throw new IOException("null btspp URL");
        }
        String n = name;
        if (n.startsWith("//")) {
            n = n.substring(2);
        } else if (n.startsWith("/")) {
            n = n.substring(1);
        }
        if (n.startsWith("localhost")) {
            return new VitaBtNotifier();
        }
        if (n.length() == 0) {
            throw new IOException("empty btspp target");
        }
        return new VitaBtConnection();
    }
}
