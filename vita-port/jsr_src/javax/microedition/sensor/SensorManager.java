/*
 * SensorManager.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * findSensors() works off a small built-in list (see VitaSensor.java). There is
 * deliberately no openSensorConnection() here: the specification opens sensors
 * through Connector.open("sensor://..."), which is handled by
 * com.sun.midp.io.j2me.sensor.Protocol calling openInternal() below.
 */
package javax.microedition.sensor;

import java.io.IOException;
import java.util.Vector;

public final class SensorManager {

    private static final Vector listeners = new Vector();
    private static final Vector listenerInfos = new Vector();

    private SensorManager() {
    }

    private static boolean isValidContext(String c) {
        return SensorInfo.CONTEXT_TYPE_AMBIENT.equals(c)
            || SensorInfo.CONTEXT_TYPE_DEVICE.equals(c)
            || SensorInfo.CONTEXT_TYPE_USER.equals(c)
            || SensorInfo.CONTEXT_TYPE_VEHICLE.equals(c);
    }

    private static SensorInfo[] toArray(Vector v) {
        SensorInfo[] r = new SensorInfo[v.size()];
        for (int i = 0; i < r.length; i++) {
            r[i] = (SensorInfo) v.elementAt(i);
        }
        return r;
    }

    public static SensorInfo[] findSensors(String quantity,
                                          String contextType) {
        if (contextType != null && !isValidContext(contextType)) {
            throw new IllegalArgumentException("invalid contextType");
        }
        Vector v = new Vector();
        SensorInfo[] all = VitaSensor.allSensors();
        for (int i = 0; i < all.length; i++) {
            SensorInfo s = all[i];
            if (quantity != null && !quantity.equals(s.getQuantity())) {
                continue;
            }
            if (contextType != null && !contextType.equals(s.getContextType())) {
                continue;
            }
            v.addElement(s);
        }
        return toArray(v);
    }

    public static SensorInfo[] findSensors(String url) {
        if (url == null) {
            throw new NullPointerException("url");
        }
        Vector v = new Vector();
        SensorInfo[] all = VitaSensor.allSensors();
        for (int i = 0; i < all.length; i++) {
            if (VitaSensor.urlMatches(all[i], url)) {
                v.addElement(all[i]);
            }
        }
        return toArray(v);
    }

    public static void addSensorListener(SensorListener listener,
                                         SensorInfo info) {
        if (listener == null || info == null) {
            throw new NullPointerException();
        }
        addListener(listener, info);
    }

    public static void addSensorListener(SensorListener listener,
                                         String quantity) {
        if (listener == null || quantity == null) {
            throw new NullPointerException();
        }
        SensorInfo[] all = findSensors(quantity, null);
        if (all.length == 0) {
            throw new IllegalArgumentException("no sensor for quantity");
        }
        addListener(listener, all[0]);
    }

    private static void addListener(SensorListener listener, SensorInfo info) {
        synchronized (listeners) {
            if (listeners.indexOf(listener) >= 0) {
                return;         /* duplicate registration is ignored */
            }
            listeners.addElement(listener);
            listenerInfos.addElement(info);
        }
        /* The specification asks for a notification right after registration
         * describing the current state; our sensors are always available. */
        listener.sensorAvailable(info);
    }

    public static void removeSensorListener(SensorListener listener) {
        if (listener == null) {
            throw new NullPointerException();
        }
        synchronized (listeners) {
            int i = listeners.indexOf(listener);
            if (i >= 0) {
                listeners.removeElementAt(i);
                listenerInfos.removeElementAt(i);
            }
        }
    }

    /**
     * Implementation hook for com.sun.midp.io.j2me.sensor.Protocol: not part of
     * JSR-256, and the only way a protocol handler in another package can build
     * a SensorConnection (the implementation classes are package-private).
     *
     * @param name the URL with the scheme removed, e.g.
     *             "//acceleration;contextType=device;model=vita"
     */
    public static SensorConnection openInternal(String name)
            throws IOException {
        return VitaSensor.open(name);
    }
}
