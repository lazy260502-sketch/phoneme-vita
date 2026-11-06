/*
 * Unit.java - JSR-256 (see ChannelInfo.java for the scope note).
 *
 * Only the factory and toString() exist in the API; the specification requires
 * the factory to return the SAME object for the same symbol, which is what
 * makes Unit comparisons with == legal, so instances are cached.
 */
package javax.microedition.sensor;

import java.util.Hashtable;

public class Unit {

    private static final Hashtable cache = new Hashtable();
    private final String symbol;

    private Unit(String symbol) {
        this.symbol = symbol;
    }

    public static Unit getUnit(String symbol) {
        if (symbol == null) {
            throw new NullPointerException("symbol");
        }
        if (symbol.length() == 0) {
            throw new IllegalArgumentException("empty symbol");
        }
        synchronized (cache) {
            Unit u = (Unit) cache.get(symbol);
            if (u == null) {
                u = new Unit(symbol);
                cache.put(symbol, u);
            }
            return u;
        }
    }

    public String toString() {
        return symbol;
    }
}
