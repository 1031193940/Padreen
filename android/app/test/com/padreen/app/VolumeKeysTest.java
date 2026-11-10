package com.padreen.app;

/** JVM test of the volume-key rule (run with android/app/test/run-tests.ps1). */
public class VolumeKeysTest {
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

    static String name(int d) {
        return d == VolumeKeys.PASS ? "PASS" : d == VolumeKeys.SWALLOW ? "SWALLOW" : d == VolumeKeys.SEND ? "SEND" : "?" + d;
    }

    static void expect(VolumeKeys k, boolean toPc, boolean linked, boolean live, boolean down, long t, int want, String what) {
        int got = k.decide(toPc, linked, live, down, t);
        check(got == want, what + ": expected " + name(want) + ", got " + name(got));
    }

    public static void main(String[] args) {
        test("the user's switch is off: the system handles every event of the keys (tablet volume)", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                long t = 1000;
                for (int linked = 0; linked < 2; linked++) {
                    for (int live = 0; live < 2; live++) {
                        for (int down = 0; down < 2; down++) {
                            expect(k, false, linked == 1, live == 1, down == 1, t += 100, VolumeKeys.PASS, "switch off, linked=" + linked + " live=" + live + " down=" + down);
                        }
                    }
                }
            }
        });

        test("no PC at all (no session, not reconnecting): the tablet's volume keys are not dead", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, false, false, true, 1000, VolumeKeys.PASS, "key press without a PC");
                expect(k, true, false, false, false, 1050, VolumeKeys.PASS, "key release without a PC");
                expect(k, true, false, false, true, 1060, VolumeKeys.PASS, "repeat without a PC");
            }
        });

        test("a session is running: the press goes to the PC, the release is only swallowed", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, true, true, 5000, VolumeKeys.SEND, "press");
                expect(k, true, true, true, false, 5120, VolumeKeys.SWALLOW, "release");
                expect(k, true, true, true, true, 6000, VolumeKeys.SEND, "next press");
                expect(k, true, true, true, false, 6090, VolumeKeys.SWALLOW, "next release");
            }
        });

        test("holding the key: at most one key per 60 ms", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, true, true, 1000, VolumeKeys.SEND, "first press");
                expect(k, true, true, true, true, 1030, VolumeKeys.SWALLOW, "repeat after 30 ms");
                expect(k, true, true, true, true, 1059, VolumeKeys.SWALLOW, "repeat after 59 ms");
                expect(k, true, true, true, true, 1060, VolumeKeys.SEND, "repeat after 60 ms");
                expect(k, true, true, true, true, 1100, VolumeKeys.SWALLOW, "repeat 40 ms after the last sent key (swallowed ones do not count)");
                expect(k, true, true, true, true, 1120, VolumeKeys.SEND, "repeat 60 ms after the last sent key");
            }
        });

        test("the very first key after boot is not rate limited", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, true, true, 5, VolumeKeys.SEND, "press 5 ms after boot");
            }
        });

        test("Wi-Fi link being re-established (grace period): the key is swallowed, nothing is sent, the limiter is untouched", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, false, true, 2000, VolumeKeys.SWALLOW, "press while reconnecting");
                expect(k, true, true, false, false, 2040, VolumeKeys.SWALLOW, "release while reconnecting");
                expect(k, true, true, true, true, 2050, VolumeKeys.SEND, "the link is back: the next press is sent at once (not limited by the swallowed one)");
                expect(k, true, true, false, true, 2060, VolumeKeys.SWALLOW, "reconnecting again (no channel): swallowed");
                expect(k, true, true, true, true, 2100, VolumeKeys.SWALLOW, "link back, but only 50 ms after the last key that was really sent");
                expect(k, true, true, true, true, 2110, VolumeKeys.SEND, "link back, 60 ms after the last key that was really sent");
            }
        });

        test("the link ends: the system handles the keys again at once", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, true, true, 3000, VolumeKeys.SEND, "press during the session");
                expect(k, true, false, false, true, 3100, VolumeKeys.PASS, "press after the session and the grace period are over");
                expect(k, true, false, false, false, 3150, VolumeKeys.PASS, "release after that");
            }
        });

        test("the switch is read for every event, never remembered", new Runnable() {
            public void run() {
                VolumeKeys k = new VolumeKeys();
                expect(k, true, true, true, true, 4000, VolumeKeys.SEND, "on");
                expect(k, false, true, true, true, 4010, VolumeKeys.PASS, "turned off on the home screen: the next event belongs to the tablet");
                expect(k, false, true, true, true, 4500, VolumeKeys.PASS, "still off");
                expect(k, true, true, true, true, 4600, VolumeKeys.SEND, "turned on again");
            }
        });

        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
