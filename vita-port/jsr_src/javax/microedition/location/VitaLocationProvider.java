/*
 * VitaLocationProvider.java - the only LocationProvider on this port.
 *
 * No GPS exists on a PS Vita, so this reports a FIXED position. The value is
 * deliberately a well-known city rather than (0,0): 0,0 reads as "no fix" to
 * many games (it is in the Gulf of Guinea), whereas a real city plus a small
 * accuracy figure makes distance/bearing code produce sane numbers.
 *
 *   latitude   39.9042 N   (Beijing)
 *   longitude 116.4074 E
 *   altitude       43 m
 *   accuracy    30 m horizontal / 50 m vertical
 *
 * getLocation() returns IMMEDIATELY. The specification allows a provider to
 * answer in less than the requested timeout, and blocking for a timeout a game
 * passed as 60 s would freeze the MIDlet's event thread - the opposite of what
 * this stub is for.
 *
 * setLocationListener() calls the listener once, synchronously, because the
 * position is already known. A real provider would post updates later; a MIDlet
 * that waits for the first callback would otherwise wait forever.
 */
package javax.microedition.location;

final class VitaLocationProvider extends LocationProvider {

    private static final double LAT = 39.9042;
    private static final double LON = 116.4074;
    private static final float ALT = 43.0f;
    private static final float HACC = 30.0f;
    private static final float VACC = 50.0f;

    private LocationListener listener;

    VitaLocationProvider() {
    }

    private static Location make() {
        QualifiedCoordinates q =
                new QualifiedCoordinates(LAT, LON, ALT, HACC, VACC);
        Location l = new Location();
        l.vitaInit(q, System.currentTimeMillis(), 0.0f, 0.0f);
        return l;
    }

    public Location getLocation(int timeout) {
        return make();
    }

    public Location getLastKnownLocation() {
        return make();
    }

    public void setLocationListener(LocationListener l, int interval,
                                    int timeout, int maxAge) {
        listener = l;
        if (l != null) {
            l.locationUpdated(this, make());
            l.providerStateChanged(this, AVAILABLE);
        }
    }

    public int getState() {
        return AVAILABLE;
    }

    public void reset() {
        listener = null;
    }
}
