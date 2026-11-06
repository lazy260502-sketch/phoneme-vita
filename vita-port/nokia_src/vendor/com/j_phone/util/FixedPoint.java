package com.j_phone.util;

/**
 * J-Phone util.FixedPoint (16.16 fixed point as exposed by the J-Phone API).
 * Behaviour follows J2ME-Loader's class (Apache-2.0), which keeps the value
 * in a double. CLDC 1.1 here has only sin/cos/tan/sqrt in java.lang.Math, so
 * atan/asin/acos are computed in this class (see atanD) and pow() is a plain
 * square, which is all Loader's Math.pow(value, 2) does.
 */
public class FixedPoint {
    private double value;

    public FixedPoint() {
    }

    public FixedPoint(int value) {
        this(value / 65536.0);
    }

    public FixedPoint(double value) {
        this.value = value;
    }

    public static FixedPoint getMaximum() {
        return new FixedPoint(Double.MAX_VALUE);
    }

    public static FixedPoint getMinimum() {
        return new FixedPoint(Double.MIN_VALUE);
    }

    /** arctangent, |error| below 1e-12 over the whole range */
    static double atanD(double x) {
        if (x != x) {
            return x;                       /* NaN */
        }
        boolean neg = x < 0;
        if (neg) {
            x = -x;
        }
        double r;
        if (x > 1e15) {
            r = Math.PI / 2;
        } else {
            boolean inv = x > 1.0;          /* atan(x) = pi/2 - atan(1/x) */
            if (inv) {
                x = 1.0 / x;
            }
            /* halve the argument twice: atan(x) = 2 * atan(x / (1 + sqrt(1 + x*x))) */
            x = x / (1.0 + Math.sqrt(1.0 + x * x));
            x = x / (1.0 + Math.sqrt(1.0 + x * x));
            /* now x <= tan(pi/16) ~= 0.199: Taylor series converges fast */
            double x2 = x * x;
            double term = x;
            double sum = x;
            for (int n = 3; n < 40; n += 2) {
                term = -term * x2;
                sum += term / n;
            }
            r = 4.0 * sum;
            if (inv) {
                r = Math.PI / 2 - r;
            }
        }
        return neg ? -r : r;
    }

    static double asinD(double v) {
        if (v > 1.0 || v < -1.0 || v != v) {
            return Double.NaN;
        }
        if (v == 1.0) {
            return Math.PI / 2;
        }
        if (v == -1.0) {
            return -Math.PI / 2;
        }
        return atanD(v / Math.sqrt(1.0 - v * v));
    }

    static double acosD(double v) {
        if (v > 1.0 || v < -1.0 || v != v) {
            return Double.NaN;
        }
        return Math.PI / 2 - asinD(v);
    }

    public FixedPoint acos(FixedPoint v) {
        value = acosD(v.value);
        return this;
    }

    public FixedPoint add(FixedPoint n) {
        value += n.value;
        return this;
    }

    public FixedPoint add(int n) {
        value += n / 65536.0;
        return this;
    }

    public FixedPoint asin(FixedPoint v) {
        value = asinD(v.value);
        return this;
    }

    public FixedPoint atan(FixedPoint v) {
        value = atanD(v.value);
        return this;
    }

    public FixedPoint clone() {
        return new FixedPoint(value);
    }

    public FixedPoint cos(FixedPoint r) {
        value = Math.cos(r.value);
        return this;
    }

    public FixedPoint divide(FixedPoint n) {
        value /= n.value;
        return this;
    }

    public FixedPoint divide(int n) {
        value /= n / 65536.0;
        return this;
    }

    public int getDecimal() {
        int v = (int) ((value - (int) value) * 65536.0);
        if (value < 0.0) {
            return -(v | 0xffff);
        } else {
            return (v & 0xffff);
        }
    }

    public int getInteger() {
        return (int) value;
    }

    public FixedPoint inverse() {
        value = 1 / value;
        return this;
    }

    public boolean isInfinite() {
        return Double.isInfinite(value);
    }

    public FixedPoint multiply(FixedPoint n) {
        value *= n.value;
        return this;
    }

    public FixedPoint multiply(int n) {
        value *= n / 65536.0;
        return this;
    }

    /** square (Loader: Math.pow(value, 2)) */
    public FixedPoint pow() {
        value = value * value;
        return this;
    }

    public void setValue(int value) {
        this.value = value / 65536.0;
    }

    public FixedPoint sin(FixedPoint r) {
        value = Math.sin(r.value);
        return this;
    }

    public FixedPoint sqrt() {
        value = Math.sqrt(value);
        return this;
    }

    public FixedPoint subtract(FixedPoint n) {
        value -= n.value;
        return this;
    }

    public FixedPoint subtract(int n) {
        value -= n / 65536.0;
        return this;
    }

    public FixedPoint tan(FixedPoint r) {
        value = Math.tan(r.value);
        return this;
    }
}
