package com.padreen.app;

import java.util.ArrayList;
import java.util.List;

/** JVM test of the accelerometer orientation detector (run with android/app/test/run-tests.ps1). */
public class OrientationDetectorTest {
    static int failures = 0;
    static int checks = 0;

    static void check(boolean ok, String what) {
        checks++;
        if (!ok) {
            failures++;
            System.out.println("  FAIL: " + what);
        }
    }

    static void test(String name, Runnable r) {
        System.out.println("- " + name);
        r.run();
    }

    static final double G = OrientationDetector.GRAVITY;

    /** What the accelerometer reads when the tablet is held `tiltDeg` from flat (90 = upright) with "up" at `phiDeg` (the detector's definition of phi). */
    static float[] vec(double phiDeg, double tiltDeg) {
        double inPlane = G * Math.sin(Math.toRadians(tiltDeg));
        double z = G * Math.cos(Math.toRadians(tiltDeg));
        return new float[]{(float) (inPlane * Math.sin(Math.toRadians(phiDeg))), (float) (inPlane * Math.cos(Math.toRadians(phiDeg))), (float) z};
    }

    /** Feeds samples at a fixed rate and remembers every change the detector asks for: {time, rotation}. */
    static final class Sim {
        final OrientationDetector d;
        final long step;
        long t = 0;
        final List<long[]> events = new ArrayList<long[]>();
        long noiseState = 12345;

        Sim(int rotation, long stepMs) {
            d = new OrientationDetector(rotation);
            step = stepMs;
        }

        int sample(float x, float y, float z) {
            int r = d.onSample(x, y, z, t);
            if (r >= 0) events.add(new long[]{t, r});
            t += step;
            return r;
        }

        int sample(double phi, double tilt) {
            float[] v = vec(phi, tilt);
            return sample(v[0], v[1], v[2]);
        }

        /** a little deterministic sensor noise, +-amp m/s^2 */
        float noise(double amp) {
            noiseState = noiseState * 6364136223846793005L + 1442695040888963407L;
            return (float) (((noiseState >>> 33) % 2001 - 1000) / 1000.0 * amp);
        }

        void hold(double phi, double tilt, long ms) {
            long end = t + ms;
            while (t < end) sample(phi, tilt);
        }

        void holdNoisy(double phi, double tilt, long ms, double amp) {
            long end = t + ms;
            while (t < end) {
                float[] v = vec(phi, tilt);
                sample(v[0] + noise(amp), v[1] + noise(amp), v[2] + noise(amp));
            }
        }

        /** a steady turn from phi `from` to `to` (degrees, the short way as given) taking `ms` */
        void turn(double from, double to, double tilt, long ms) {
            long n = Math.max(1, ms / step);
            for (long i = 1; i <= n; i++) sample(from + (to - from) * i / n, tilt);
        }

        String ev() {
            StringBuilder sb = new StringBuilder("[");
            for (long[] e : events) sb.append(e[0]).append("ms:").append(e[1]).append(' ');
            return sb.append(']').toString();
        }

        int count() {
            return events.size();
        }

        long[] last() {
            return events.isEmpty() ? new long[]{-1, -1} : events.get(events.size() - 1);
        }
    }

    public static void main(String[] args) {
        test("first sample initialises the filter; a tablet held upright in the current orientation stays put", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.holdNoisy(0, 90, 5000, 0.15);
                check(s.count() == 0 && s.d.current() == 0, "no change for 5 s of upright portrait with sensor noise: " + s.ev());
                Sim l = new Sim(1, 20);
                l.holdNoisy(90, 90, 5000, 0.15);
                check(l.count() == 0 && l.d.current() == 1, "landscape stays landscape: " + l.ev());
                Sim q = new Sim(0, 20);
                q.sample(90, 90);                                       // the very first sample already says landscape: no filter ramp-up to wait for
                for (int i = 0; i < 30; i++) q.sample(90, 90);
                check(q.count() == 1 && q.last()[1] == 1 && q.last()[0] == OrientationDetector.SETTLE_MS + 10, "settles SETTLE_MS after the first sample (to the next sample): " + q.ev());
            }
        });

        test("a smooth 90 degree turn over about a second: exactly one change, after crossing ~52 degrees plus the settle time", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 1000);
                s.turn(0, 90, 90, 1000);
                s.hold(90, 90, 2000);
                double crossing = 1000 + (45 + OrientationDetector.HYST_DEG) / 90.0 * 1000;
                check(s.count() == 1 && s.last()[1] == 1, "one change to landscape: " + s.ev());
                check(s.count() == 1 && s.last()[0] >= crossing + OrientationDetector.SETTLE_MS && s.last()[0] <= crossing + OrientationDetector.SETTLE_MS + 300,
                        "not before crossing + settle (" + (crossing + OrientationDetector.SETTLE_MS) + " ms), not much later (filter lag): " + s.ev());
                check(s.d.current() == 1, "current() follows");
            }
        });

        test("turning back to portrait", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                s.turn(0, 90, 90, 1000);
                s.hold(90, 90, 2000);
                s.turn(90, 0, 90, 1000);
                s.hold(0, 90, 2000);
                check(s.count() == 2 && s.events.get(0)[1] == 1 && s.events.get(1)[1] == 0, "landscape, then portrait: " + s.ev());
                check(s.d.current() == 0, "current() is portrait again");
            }
        });

        test("reverse landscape (turned clockwise)", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                s.turn(0, -90, 90, 1000);
                s.hold(-90, 90, 2000);
                check(s.count() == 1 && s.last()[1] == 3 && s.d.current() == 3, "one change to reverse landscape: " + s.ev());
            }
        });

        test("reverse portrait (upside down), from portrait and from landscape", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                s.turn(0, 180, 90, 1000);                           // a slow half turn: passes through the landscape zone long enough to switch there first
                s.hold(180, 90, 3000);
                check(s.count() >= 1 && s.last()[1] == 2 && s.d.current() == 2, "ends in reverse portrait: " + s.ev());
                check(s.count() <= 2, "at most one intermediate stop: " + s.ev());
                Sim f = new Sim(0, 20);
                f.hold(0, 90, 500);
                f.turn(0, 180, 90, 300);                            // a quick flip does not dwell in the landscape zone
                f.hold(180, 90, 2000);
                check(f.count() == 1 && f.last()[1] == 2, "a quick flip goes straight there: " + f.ev());
                Sim l = new Sim(1, 20);
                l.hold(90, 90, 500);
                l.turn(90, 180, 90, 500);
                l.hold(180, 90, 2000);
                check(l.count() == 1 && l.last()[1] == 2, "landscape -> reverse portrait: " + l.ev());
                Sim w = new Sim(0, 20);
                w.hold(0, 90, 500);
                w.turn(0, -180, 90, 300);                           // the other way round
                w.hold(-180, 90, 2000);
                check(w.count() == 1 && w.last()[1] == 2, "turned the other way round to reverse portrait: " + w.ev());
            }
        });

        test("landscape <-> reverse landscape flips, both directions, through either side", new Runnable() {
            public void run() {
                // landscape -> reverse landscape through portrait (90 -> 0 -> -90) and back
                Sim a = new Sim(1, 20);
                a.hold(90, 90, 500);
                a.turn(90, -90, 90, 400);
                a.hold(-90, 90, 3000);
                check(a.d.current() == 3 && a.count() >= 1 && a.last()[1] == 3 && a.count() <= 2, "landscape -> reverse landscape: " + a.ev());
                a.turn(-90, 90, 90, 400);
                a.hold(90, 90, 3000);
                check(a.d.current() == 1 && a.last()[1] == 1, "and back: " + a.ev());
                // ... through reverse portrait (90 -> 180 -> 270)
                Sim b = new Sim(1, 20);
                b.hold(90, 90, 500);
                b.turn(90, 270, 90, 400);
                b.hold(270, 90, 3000);
                check(b.d.current() == 3 && b.last()[1] == 3 && b.count() <= 2, "landscape -> reverse landscape the other way round: " + b.ev());
                b.turn(270, 90, 90, 400);
                b.hold(90, 90, 3000);
                check(b.d.current() == 1 && b.last()[1] == 1, "and back: " + b.ev());
                // an abrupt flip (one sample says the opposite): still one clean change
                Sim c = new Sim(1, 20);
                c.hold(90, 90, 500);
                c.hold(-90, 90, 3000);
                check(c.count() == 1 && c.last()[1] == 3, "abrupt flip: " + c.ev());
                c.hold(90, 90, 3000);
                check(c.count() == 2 && c.last()[1] == 1, "abrupt flip back: " + c.ev());
            }
        });

        test("a tablet lying flat never switches (its 'up' is meaningless)", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.holdNoisy(0, 0, 10000, 0.4);                      // z = g, x / y are only noise
                check(s.count() == 0, "flat on a table, noisy: " + s.ev());
                Sim u = new Sim(0, 20);
                u.sample(0.5f, 0.5f, 9.8f);
                for (int i = 0; i < 400; i++) u.sample(1.1f, 0.0f, 9.75f);       // in-plane 1.1 m/s^2
                check(u.count() == 0, "a tiny constant slope: " + u.ev());
                Sim t9 = new Sim(0, 20);
                t9.hold(0, 90, 300);
                t9.hold(90, 9, 3000);                                // tilted 9 degrees only, "up" would be landscape
                check(t9.count() == 0, "9 degrees from flat: " + t9.ev());
                Sim t12 = new Sim(0, 20);
                t12.hold(0, 90, 300);
                t12.hold(90, 12, 3000);
                check(t12.count() == 1 && t12.last()[1] == 1, "12 degrees from flat already switches (the Android default needs ~27): " + t12.ev());
            }
        });

        test("a tablet held at 30..60 degrees tilt still switches", new Runnable() {
            public void run() {
                double[] tilts = {30, 45, 60, 75};
                for (double tilt : tilts) {
                    Sim s = new Sim(0, 20);
                    s.hold(0, tilt, 500);
                    s.turn(0, 90, tilt, 800);
                    s.hold(90, tilt, 1500);
                    check(s.count() == 1 && s.last()[1] == 1, "tilt " + tilt + ": " + s.ev());
                    s.turn(90, 0, tilt, 800);
                    s.hold(0, tilt, 2500);
                    check(s.count() == 2 && s.last()[1] == 0, "tilt " + tilt + " and back: " + s.ev());
                }
            }
        });

        test("shaking (large acceleration) never switches", new Runnable() {
            public void run() {
                double[] freqs = {1.5, 2, 3, 4, 6, 8};
                double[] amps = {12, 20, 30};
                for (double f : freqs) {
                    for (double amp : amps) {
                        for (int axis = 0; axis < 3; axis++) {
                            Sim s = new Sim(0, 20);
                            s.hold(0, 90, 500);
                            int bad = 0;
                            for (long t = 0; t < 6000; t += 20) {
                                double w = Math.sin(2 * Math.PI * f * t / 1000.0) * amp;
                                float x = 0, y = (float) G, z = 0;
                                if (axis == 0) x += (float) w;                        // side to side
                                else if (axis == 1) y += (float) w;                   // up and down
                                else {                                                // diagonal
                                    x += (float) (w * 0.7);
                                    y += (float) (w * 0.7);
                                }
                                s.sample(x, y, z);
                            }
                            if (s.count() != 0) {
                                bad++;
                                check(false, "shake " + f + " Hz, " + amp + " m/s^2, axis " + axis + " switched: " + s.ev());
                            }
                            check(bad == 0, "shake " + f + " Hz / " + amp + " / axis " + axis);
                        }
                    }
                }
            }
        });

        test("being carried / dropped (|a| far from g) decides nothing, then the tablet is believed again once it is steady", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                for (int i = 0; i < 150; i++) s.sample(15f, 12f, 0f);   // 19 m/s^2 for 3 s: "landscape-ish" but far too strong to be gravity
                check(s.count() == 0, "strong acceleration is ignored: " + s.ev());
                for (int i = 0; i < 40; i++) s.sample(0f, 0f, 0f);       // free fall
                check(s.count() == 0, "free fall is ignored: " + s.ev());
                s.hold(90, 90, 2000);
                check(s.count() == 1 && s.last()[1] == 1, "steady again: switches normally: " + s.ev());
                // a strong constant pull that points clearly into another zone would switch if |a| were not checked
                Sim a = new Sim(0, 20);
                a.hold(0, 90, 500);
                for (int i = 0; i < 200; i++) a.sample(25f, 3f, 0f);
                check(a.count() == 0, "25 m/s^2 towards landscape is not gravity: " + a.ev());
                for (int i = 0; i < 200; i++) a.sample(-22f, 2f, 0f);
                check(a.count() == 0, "22 m/s^2 towards reverse landscape is not gravity: " + a.ev());
                // the tolerance edge in steady state (the very first sample sets the filter, so there is no transient): | |a| - g | <= 2.5 counts, beyond does not
                double[] mags = {G + 2.6, G - 2.6, G + 2.0, G - 2.0, G + 6, G - 6};
                boolean[] counts = {false, false, true, true, false, false};
                for (int i = 0; i < mags.length; i++) {
                    Sim e = new Sim(0, 20);
                    for (int k = 0; k < 100; k++) e.sample((float) mags[i], 0f, 0f);       // "up" is landscape, with this length
                    boolean switched = e.count() == 1 && e.last()[1] == 1;
                    check(switched == counts[i], "|a| = " + mags[i] + (counts[i] ? " is taken as gravity: " : " is not taken as gravity: ") + e.ev());
                }
                // braking in a car: gravity plus 0.4 g sideways stays inside the zone, nothing happens
                Sim c = new Sim(0, 20);
                c.hold(0, 90, 300);
                for (int i = 0; i < 200; i++) c.sample(3.9f, 9.81f, 0f);
                check(c.count() == 0, "a sideways push of 0.4 g does not turn the display: " + c.ev());
            }
        });

        test("jitter around the 45 degree boundary does not flap (hysteresis)", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                for (int i = 0; i < 400; i++) s.sample(i % 2 == 0 ? 40 : 50, 90);     // alternating 40 / 50 degrees for 8 s
                check(s.count() == 0, "portrait keeps its zone up to 52 degrees: " + s.ev());
                for (long t = 0; t < 8000; t += 20) s.sample(45 + 7 * Math.sin(2 * Math.PI * 2 * t / 1000.0), 90);   // slow wobble 38..52 degrees
                check(s.count() == 0, "a wobble of +-7 degrees around 45 degrees: " + s.ev());
                s.hold(90, 90, 1500);                                       // a real turn to landscape
                check(s.count() == 1 && s.last()[1] == 1, "then a real turn switches: " + s.ev());
                for (int i = 0; i < 400; i++) s.sample(i % 2 == 0 ? 40 : 50, 90);
                check(s.count() == 1, "landscape keeps its zone down to 38 degrees (no flap back): " + s.ev());
                for (long t = 0; t < 8000; t += 20) s.sample(45 + 7 * Math.sin(2 * Math.PI * 2 * t / 1000.0), 90);
                check(s.count() == 1, "the same wobble now: " + s.ev());
                // jitter right at the outer boundary (52 degrees): once it switches, the new zone holds it (no flapping back and forth)
                Sim j = new Sim(0, 20);
                j.hold(0, 90, 500);
                for (int i = 0; i < 600; i++) j.sample(i % 2 == 0 ? 49 : 55, 90);
                check(j.count() <= 1 && (j.count() == 0 || j.last()[1] == 1), "jitter around 52 degrees: at most one change, never a flap: " + j.ev());
                Sim k = new Sim(0, 20);
                k.hold(0, 90, 500);
                for (int i = 0; i < 600; i++) k.sample(i % 2 == 0 ? 46 : 52, 90);        // mean 49 degrees: below the boundary, so it never leaves
                check(k.count() == 0, "jitter just below the boundary never switches: " + k.ev());
            }
        });

        test("the cool-down between two changes is honoured", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 500);
                s.hold(90, 90, 1000);
                check(s.count() == 1 && s.last()[1] == 1, "first change: " + s.ev());
                long first = s.last()[0];
                s.hold(0, 90, 1200);                                        // snaps straight back to portrait
                check(s.count() == 2 && s.last()[1] == 0, "second change happens: " + s.ev());
                check(s.last()[0] - first >= OrientationDetector.COOLDOWN_MS, "not earlier than COOLDOWN_MS after the first: " + s.ev());
                check(s.last()[0] - first <= OrientationDetector.COOLDOWN_MS + 100, "and as soon as the cool-down is over (the candidate was waiting): " + s.ev());
            }
        });

        test("setCurrent tells the detector where the activity is, and cancels a candidate", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(90, 90, 200);                                        // a candidate (landscape) is 200 ms into its 250 ms
                s.d.setCurrent(3);                                          // the PC locked the activity elsewhere meanwhile
                check(s.d.current() == 3, "current() is what was set");
                long since = s.t;
                s.hold(90, 90, 600);
                check(s.count() == 1 && s.last()[1] == 1, "still switches to what the sensor says: " + s.ev());
                check(s.last()[0] >= since + OrientationDetector.SETTLE_MS, "but only after a full settle time counted from setCurrent: " + s.ev());
                Sim u = new Sim(0, 20);
                u.hold(90, 90, 200);
                u.d.setCurrent(1);                                          // the activity is landscape now: nothing left to do
                u.hold(90, 90, 3000);
                check(u.count() == 0 && u.d.current() == 1, "no change when the new current rotation already is the sensor's: " + u.ev());
                Sim w = new Sim(2, 20);
                w.d.setCurrent(-1);
                check(w.d.current() == 3, "setCurrent normalises (-1 -> 3)");
                w.d.setCurrent(6);
                check(w.d.current() == 2, "setCurrent normalises (6 -> 2)");
            }
        });

        test("broken or odd samples", new Runnable() {
            public void run() {
                Sim s = new Sim(0, 20);
                s.hold(0, 90, 300);
                for (int i = 0; i < 100; i++) s.sample(Float.NaN, 5f, 5f);
                s.sample(Float.POSITIVE_INFINITY, 0f, 0f);
                check(s.count() == 0, "NaN / infinity decide nothing: " + s.ev());
                s.hold(90, 90, 1500);
                check(s.count() == 1 && s.last()[1] == 1, "and do not poison the filter: " + s.ev());
                Sim n = new Sim(0, 20);
                check(n.d.onSample(Float.NaN, Float.NaN, Float.NaN, 0) == -1 && Double.isNaN(n.d.phiDeg()), "NaN as the very first sample: no data yet");
                n.hold(90, 90, 1000);
                check(n.count() == 1, "the first good sample then starts the filter: " + n.ev());
                Sim o = new Sim(0, 20);
                o.d.onSample(0f, 9.81f, 0f, 1000);
                o.d.onSample(0f, 9.81f, 0f, 500);                           // the clock jumps backwards
                o.d.onSample(0f, 9.81f, 0f, 1000);
                check(o.d.current() == 0 && o.d.phiDeg() == 0.0, "a clock that runs backwards is harmless");
                Sim p = new Sim(0, 20);
                p.d.onSample(0f, 9.81f, 0f, 0);
                p.d.onSample(9.81f, 0f, 0f, 3600000);                       // one sample an hour later: the step is clamped to DT_MAX_MS
                double phi = p.d.phiDeg();
                check(phi > 40 && phi < 80, "a long gap moves the filter only part of the way (alpha = 200 / (120 + 200)): phi=" + phi);
            }
        });

        test("static helpers", new Runnable() {
            public void run() {
                check(OrientationDetector.rotationOfAngle(0) == 0 && OrientationDetector.rotationOfAngle(44) == 0 && OrientationDetector.rotationOfAngle(-44) == 0, "zone 0");
                check(OrientationDetector.rotationOfAngle(46) == 1 && OrientationDetector.rotationOfAngle(90) == 1 && OrientationDetector.rotationOfAngle(134) == 1, "zone 1");
                check(OrientationDetector.rotationOfAngle(136) == 2 && OrientationDetector.rotationOfAngle(180) == 2 && OrientationDetector.rotationOfAngle(-180) == 2
                        && OrientationDetector.rotationOfAngle(-136) == 2, "zone 2");
                check(OrientationDetector.rotationOfAngle(-46) == 3 && OrientationDetector.rotationOfAngle(-90) == 3 && OrientationDetector.rotationOfAngle(270) == 3, "zone 3");
                check(OrientationDetector.wrap(190) == -170 && OrientationDetector.wrap(-190) == 170 && OrientationDetector.wrap(180) == 180 && OrientationDetector.wrap(-180) == 180
                        && OrientationDetector.wrap(720) == 0, "wrap");
                check(OrientationDetector.angularDistance(-170, 170) == 20 && OrientationDetector.angularDistance(10, -10) == 20, "angular distance across +-180 / 0");
                check(OrientationDetector.isLandscape(1) && OrientationDetector.isLandscape(3) && !OrientationDetector.isLandscape(0) && !OrientationDetector.isLandscape(2), "shapes");
                check(OrientationDetector.rotationForShape(true, 3) == 3 && OrientationDetector.rotationForShape(true, 1) == 1 && OrientationDetector.rotationForShape(true, 0) == 1
                        && OrientationDetector.rotationForShape(true, 2) == 1, "landscape wanted: keep reverse landscape, else landscape");
                check(OrientationDetector.rotationForShape(false, 2) == 2 && OrientationDetector.rotationForShape(false, 0) == 0 && OrientationDetector.rotationForShape(false, 1) == 0
                        && OrientationDetector.rotationForShape(false, 3) == 0, "portrait wanted: keep reverse portrait, else portrait");
                check("portrait".equals(OrientationDetector.name(0)) && "landscape".equals(OrientationDetector.name(1)) && "reverse portrait".equals(OrientationDetector.name(2))
                        && "reverse landscape".equals(OrientationDetector.name(3)), "names");
            }
        });

        test("startRotation: a new display activity starts in the remembered rotation, unless the PC or the user's switch dictates the shape", new Runnable() {
            public void run() {
                for (int r = 0; r < 4; r++) {
                    check(OrientationDetector.startRotation(0, true, r) == r, "auto rotate, no lock: the remembered rotation " + r + " is kept");
                }
                check(OrientationDetector.startRotation(0, true, 5) == 1 && OrientationDetector.startRotation(0, true, -1) == 3, "a remembered value is normalised");
                // the PC's lock wins: 1 = landscape (keep reverse landscape, else landscape), 2 = portrait (keep reverse portrait, else portrait)
                check(OrientationDetector.startRotation(1, true, 3) == 3 && OrientationDetector.startRotation(1, true, 1) == 1 && OrientationDetector.startRotation(1, true, 0) == 1
                        && OrientationDetector.startRotation(1, true, 2) == 1, "PC locked to landscape");
                check(OrientationDetector.startRotation(2, true, 2) == 2 && OrientationDetector.startRotation(2, true, 0) == 0 && OrientationDetector.startRotation(2, true, 1) == 0
                        && OrientationDetector.startRotation(2, true, 3) == 0, "PC locked to portrait");
                check(OrientationDetector.startRotation(2, false, 1) == 0 && OrientationDetector.startRotation(1, false, 0) == 1, "the PC's lock also wins over auto rotate being off");
                // auto rotate off: landscape, in the direction that was remembered
                check(OrientationDetector.startRotation(0, false, 3) == 3 && OrientationDetector.startRotation(0, false, 1) == 1 && OrientationDetector.startRotation(0, false, 0) == 1
                        && OrientationDetector.startRotation(0, false, 2) == 1, "auto rotate off: landscape, same direction if already landscape");
            }
        });

        test("fits: only a rotation of the same shape as the surface is worth remembering", new Runnable() {
            public void run() {
                check(OrientationDetector.fits(1, 1872, 1404) && OrientationDetector.fits(3, 1872, 1404) && OrientationDetector.fits(0, 1404, 1872) && OrientationDetector.fits(2, 1404, 1872),
                        "the four rotations with their own shapes");
                check(!OrientationDetector.fits(1, 1404, 1872) && !OrientationDetector.fits(3, 1404, 1872) && !OrientationDetector.fits(0, 1872, 1404) && !OrientationDetector.fits(2, 1872, 1404),
                        "a rotation one step behind the surface");
                check(!OrientationDetector.fits(0, 1000, 1000) && !OrientationDetector.fits(1, 0, 0), "a square or empty surface says nothing");
            }
        });

        test("a detector started in the remembered rotation keeps it on a tablet lying flat - whichever rotation that was", new Runnable() {
            public void run() {
                for (int r = 0; r < 4; r++) {
                    Sim s = new Sim(OrientationDetector.startRotation(0, true, r), 20);
                    s.holdNoisy(0, 0, 8000, 0.3);                           // flat on the desk, only sensor noise in the screen plane
                    check(s.count() == 0 && s.d.current() == r, "flat, remembered rotation " + r + " stays: " + s.ev());
                    Sim t = new Sim(r, 20);
                    t.hold(60, 8, 5000);                                    // lying almost flat (8 degrees), the little slope is not a decision
                    t.hold(-120, 8, 5000);
                    check(t.count() == 0 && t.d.current() == r, "almost flat (8 degrees) with the slope pointing anywhere: rotation " + r + " stays: " + t.ev());
                }
                Sim u = new Sim(0, 20);                                     // picked up and held upright in landscape: now the sensor has an opinion
                u.holdNoisy(0, 0, 2000, 0.3);
                u.hold(90, 70, 1500);
                check(u.count() == 1 && u.last()[1] == 1, "lifted and held landscape: it follows: " + u.ev());
            }
        });

        test("axis convention: the readings of a tablet in the four grips map to the four rotations", new Runnable() {
            public void run() {
                // natural portrait upright: +g on y; turned counter-clockwise (landscape, rotation 1): +g on x; reverse landscape: -g on x; upside down: -g on y
                Sim p = new Sim(2, 20);
                for (int i = 0; i < 40; i++) p.sample(0f, 9.81f, 0f);
                check(p.count() == 1 && p.last()[1] == 0, "+g on y = portrait: " + p.ev());
                Sim l = new Sim(0, 20);
                for (int i = 0; i < 40; i++) l.sample(9.81f, 0f, 0f);
                check(l.count() == 1 && l.last()[1] == 1, "+g on x = landscape (rotation 1): " + l.ev());
                Sim r = new Sim(0, 20);
                for (int i = 0; i < 40; i++) r.sample(-9.81f, 0f, 0f);
                check(r.count() == 1 && r.last()[1] == 3, "-g on x = reverse landscape (rotation 3): " + r.ev());
                Sim u = new Sim(0, 20);
                for (int i = 0; i < 40; i++) u.sample(0f, -9.81f, 0f);
                check(u.count() == 1 && u.last()[1] == 2, "-g on y = reverse portrait (rotation 2): " + u.ev());
            }
        });

        test("random handling for 30 simulated minutes: every answer is a real change, never two within the cool-down, never garbage", new Runnable() {
            public void run() {
                long seed = 987654321L;
                Sim s = new Sim(0, 40);
                int expectedCurrent = 0;
                long lastChange = Long.MIN_VALUE / 4;
                boolean ok = true;
                String why = "";
                double phi = 0, tilt = 80;
                for (int segment = 0; segment < 1200 && ok; segment++) {
                    seed = seed * 6364136223846793005L + 1442695040888963407L;
                    double toPhi = ((seed >>> 33) % 360) - 180;
                    seed = seed * 6364136223846793005L + 1442695040888963407L;
                    double toTilt = 5 + ((seed >>> 33) % 86);
                    seed = seed * 6364136223846793005L + 1442695040888963407L;
                    long ms = 200 + ((seed >>> 33) % 2800);
                    seed = seed * 6364136223846793005L + 1442695040888963407L;
                    int kind = (int) ((seed >>> 33) % 10);                  // 0 shake, 1 broken samples, otherwise a normal move
                    long n = Math.max(1, ms / s.step);
                    for (long i = 1; i <= n && ok; i++) {
                        double p = phi + (toPhi - phi) * i / n, t = tilt + (toTilt - tilt) * i / n;
                        float[] v = vec(p, t);
                        float x = v[0] + s.noise(0.1), y = v[1] + s.noise(0.1), z = v[2] + s.noise(0.1);
                        if (kind == 0) {
                            x += (float) (Math.sin(i * 0.9) * 18);
                            y += (float) (Math.cos(i * 1.3) * 18);
                        } else if (kind == 1 && i % 7 == 0) {
                            x = Float.NaN;
                        }
                        long before = s.t;
                        int r = s.sample(x, y, z);
                        if (r >= 0) {
                            if (r > 3 || r == expectedCurrent) {
                                ok = false;
                                why = "answer " + r + " when current was " + expectedCurrent + " at " + before;
                            } else if (before - lastChange < OrientationDetector.COOLDOWN_MS) {
                                ok = false;
                                why = "two changes " + (before - lastChange) + " ms apart at " + before;
                            }
                            expectedCurrent = r;
                            lastChange = before;
                        }
                        if (s.d.current() != expectedCurrent) {
                            ok = false;
                            why = "current() " + s.d.current() + " != " + expectedCurrent + " at " + before;
                        }
                    }
                    phi = toPhi;
                    tilt = toTilt;
                }
                check(ok, "invariants hold: " + why);
                check(s.t > 25 * 60 * 1000L && s.count() > 20, "a long, busy run (" + s.t / 1000 + " s, " + s.count() + " changes)");
            }
        });

        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
