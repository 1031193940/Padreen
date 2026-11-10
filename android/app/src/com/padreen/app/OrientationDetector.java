package com.padreen.app;

/**
 * Which way up is the tablet held?  Decided from the accelerometer alone (the system's own orientation listener has no sensor behind it on this
 * e-ink tablet), kept free of any Android class so it can be unit-tested on the JVM.  UI thread only.
 *
 * The sensor axes belong to the tablet's NATURAL orientation (portrait): x = right, y = up the long side, z = out of the screen, and the
 * accelerometer reads +g on the axis that points UP.  From the low-pass filtered vector
 *     phi = atan2(x, y)   (degrees)
 * is the direction of "up" seen from the screen:   0 = natural portrait upright (rotation 0),  +90 = turned counter-clockwise = landscape
 * (rotation 1),  -90 = reverse landscape (rotation 3),  +-180 = reverse portrait (rotation 2).  Rotation r therefore has its zone centred on
 * 90 * r degrees.
 *
 * What makes it feel right (every number is a named constant):
 *  - the vector is smoothed (time constant FILTER_TAU_MS); a tablet that is being shaken or carried (|a| far from g) decides nothing;
 *  - a tablet lying nearly flat (in-plane gravity below FLAT_IN_PLANE, tilt under about 10 degrees) decides nothing either - its "up" is
 *    meaningless, so it keeps the orientation it shows - but anything steeper works, so it does not need the ~27 degrees the Android default asks for;
 *  - the current rotation keeps its zone until phi is more than ZONE_HALF_DEG + HYST_DEG away from its centre (hysteresis: no flapping at
 *    the 45 degree boundary); beyond that the nearest zone is the candidate, which must stay the same for SETTLE_MS, and COOLDOWN_MS must
 *    have passed since the last change.
 * Whoever turns the activity by other means (the PC's lock, the system) tells it with {@link #setCurrent}.
 */
final class OrientationDetector {
    static final double GRAVITY = 9.81;               // m/s^2
    static final long FILTER_TAU_MS = 120;            // low-pass time constant
    static final long DT_MIN_MS = 5, DT_MAX_MS = 200; // the time step of one sample is clamped to this range (odd timestamps, a gap after a pause)
    static final double MOVING_TOLERANCE = 2.5;       // m/s^2: | |a| - g | above this = being moved, no decision
    static final double FLAT_IN_PLANE = 1.7;          // m/s^2: gravity in the screen plane below this = lying flat (tilt under ~10 degrees), no decision
    static final double ZONE_HALF_DEG = 45;           // half width of a rotation's zone ...
    static final double HYST_DEG = 7;                 // ... plus this much before the current rotation gives its zone up
    static final long SETTLE_MS = 250;                // the candidate must hold this long
    static final long COOLDOWN_MS = 1000;             // at least this long between two changes

    private int current;
    private boolean haveData;
    private double fx, fy, fz;                        // filtered vector
    private long lastTime;                            // time of the latest sample (never goes backwards)
    private int candidate = -1;
    private long candidateSince;
    private boolean everChanged;
    private long lastChange;

    OrientationDetector(int initialRotation) {
        current = normalize(initialRotation);
    }

    /** The rotation (0..3, as in Display.getRotation()) the detector believes the display is in. */
    int current() {
        return current;
    }

    /** The activity was turned by other means (PC lock, the system): this is where it is now.  Cancels any candidate. */
    void setCurrent(int rotation) {
        current = normalize(rotation);
        candidate = -1;
    }

    /**
     * One accelerometer sample (m/s^2, the sensor's own axes) taken at timeMs (any steady millisecond clock).
     * @return the NEW rotation (0..3) the activity should turn to - the detector already regards it as current - or -1 for "no change"
     */
    int onSample(float x, float y, float z, long timeMs) {
        if (!finite(x) || !finite(y) || !finite(z)) {          // a broken sample decides nothing (and never poisons the filter)
            candidate = -1;
            return -1;
        }
        if (!haveData) {
            fx = x;
            fy = y;
            fz = z;
            lastTime = timeMs;
            haveData = true;
        } else {
            long t = Math.max(timeMs, lastTime);                // the clock never runs backwards here
            long dt = Math.min(DT_MAX_MS, Math.max(DT_MIN_MS, t - lastTime));
            double a = dt / (double) (FILTER_TAU_MS + dt);
            fx += a * (x - fx);
            fy += a * (y - fy);
            fz += a * (z - fz);
            lastTime = t;
        }
        long now = lastTime;
        if (Math.abs(magnitude() - GRAVITY) > MOVING_TOLERANCE) {       // shaken, carried, in a lift: nothing to see
            candidate = -1;
            return -1;
        }
        if (inPlane() < FLAT_IN_PLANE) {                                // lying flat: "up" is not defined by the sensor
            candidate = -1;
            return -1;
        }
        double phi = phiDeg();
        if (angularDistance(phi, 90.0 * current) <= ZONE_HALF_DEG + HYST_DEG) {   // still inside the current zone (plus hysteresis)
            candidate = -1;
            return -1;
        }
        int target = rotationOfAngle(phi);
        if (target != candidate) {
            candidate = target;
            candidateSince = now;
            return -1;
        }
        if (now - candidateSince < SETTLE_MS) return -1;
        if (everChanged && now - lastChange < COOLDOWN_MS) return -1;   // the candidate stays, it goes through once the cool-down is over
        current = target;
        candidate = -1;
        everChanged = true;
        lastChange = now;
        return target;
    }

    // ---- the filtered vector, for logging ------------------------------------------------------------------------------------------
    /** Direction of "up" from the filtered vector (see the class comment); NaN before the first sample. */
    double phiDeg() {
        return haveData ? Math.toDegrees(Math.atan2(fx, fy)) : Double.NaN;
    }

    /** Gravity in the screen plane (m/s^2) of the filtered vector; NaN before the first sample. */
    double inPlane() {
        return haveData ? Math.hypot(fx, fy) : Double.NaN;
    }

    /** Length of the filtered vector (m/s^2); NaN before the first sample. */
    double magnitude() {
        return haveData ? Math.sqrt(fx * fx + fy * fy + fz * fz) : Double.NaN;
    }

    /** Tilt of the screen from the horizontal in degrees: 0 = lying flat, 90 = upright; NaN before the first sample. */
    double tiltDeg() {
        return haveData ? Math.toDegrees(Math.atan2(inPlane(), Math.abs(fz))) : Double.NaN;
    }

    // ---- small pure helpers (also used by the activity) ----------------------------------------------------------------------------
    static boolean isLandscape(int rotation) {
        return (normalize(rotation) & 1) == 1;
    }

    /**
     * The rotation to use when somebody else decides the SHAPE (the PC's lock, the user's fixed choice): the current one if its shape already
     * fits - a landscape display that is upside down stays upside down, nothing flips - otherwise the usual one (1 landscape, 0 portrait).
     */
    static int rotationForShape(boolean landscape, int current) {
        int c = normalize(current);
        if (isLandscape(c) == landscape) return c;
        return landscape ? 1 : 0;
    }

    /**
     * The rotation a NEW display activity starts in (Back / the home screen destroy the old one, the PC's monitor keeps its shape): the
     * rotation the previous one was last seen in - the manifest's landscape must not turn a portrait desktop - unless somebody dictates the
     * shape: the PC's lock (1 = landscape, 2 = portrait) or "auto rotate" off (landscape); the direction is kept when the shape fits.
     * A tablet lying flat has nothing to say about it: the remembered rotation stays.
     */
    static int startRotation(int lock, boolean autoRotate, int remembered) {
        if (lock != 0) return rotationForShape(lock == 1, remembered);
        if (!autoRotate) return rotationForShape(true, remembered);
        return normalize(remembered);
    }

    /**
     * Does a display of this size belong to this rotation?  (The natural orientation is portrait: rotations 0 / 2 are portrait, 1 / 3 landscape.)
     * A rotation read while the system is still turning can be one behind the surface; such a pair is not worth remembering.
     */
    static boolean fits(int rotation, int w, int h) {
        return w != h && isLandscape(rotation) == (w > h);
    }

    static String name(int rotation) {
        switch (normalize(rotation)) {
            case 0:
                return "portrait";
            case 1:
                return "landscape";
            case 2:
                return "reverse portrait";
            default:
                return "reverse landscape";
        }
    }

    /** The zone (0..3) whose centre (90 * r degrees) is nearest to this angle. */
    static int rotationOfAngle(double phiDeg) {
        return normalize((int) Math.round(phiDeg / 90.0));
    }

    /** Angle wrapped into (-180, 180]. */
    static double wrap(double deg) {
        double d = deg % 360.0;
        if (d > 180.0) d -= 360.0;
        else if (d <= -180.0) d += 360.0;
        return d;
    }

    static double angularDistance(double a, double b) {
        return Math.abs(wrap(a - b));
    }

    static int normalize(int rotation) {
        return ((rotation % 4) + 4) % 4;
    }

    private static boolean finite(float v) {
        return !Float.isNaN(v) && !Float.isInfinite(v);
    }
}
