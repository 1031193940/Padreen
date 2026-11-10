package com.padreen.app;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** JVM test of the orientation negotiation state machine (run with android/app/test/run-tests.ps1). */
public class OrientSyncTest {
    static int failures = 0;
    static int checks = 0;

    static final class Rec implements OrientSync.Sink {
        final List<String> log = new ArrayList<String>();
        int lastSeq;

        @Override
        public void sendOrient(int w, int h, int seq) {
            log.add("orient " + w + "x" + h + "#" + seq);
            lastSeq = seq;
        }

        @Override
        public void forceOrientation(boolean landscape) {
            log.add("force " + (landscape ? "landscape" : "portrait"));
        }

        @Override
        public void releaseOrientation() {
            log.add("release");
        }

        @Override
        public void armTimeout(int seq) {
            log.add("arm#" + seq);
        }

        @Override
        public void requestFullFrame() {
            log.add("fullframe");
        }

        void clear() {
            log.clear();
        }

        boolean has(String s) {
            return log.contains(s);
        }

        /** The log is exactly this, in this order. */
        boolean is(String... expected) {
            return log.equals(Arrays.asList(expected));
        }
    }

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

    static final int LW = 1872, LH = 1404;     // landscape picture
    static final int PW = 1404, PH = 1872;     // portrait picture

    public static void main(String[] args) {
        test("no session: turning the device sends nothing and never blocks", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.onSize(PW, PH);
                check(r.log.isEmpty(), "nothing sent without a session");
                check(!s.pending(), "not pending");
            }
        });

        test("normal turn: request, frames blocked until the answer, then accepted", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, false);                  // the PC's announcement, same size
                check(r.log.isEmpty(), "announcement of the same size triggers nothing");
                s.onSize(PW, PH);
                check(r.has("orient 1404x1872#1") && r.has("arm#1"), "T_ORIENT#1 sent and the timeout armed: " + r.log);
                check(s.pending(), "pending after the turn");
                s.onGeom(PW, PH, 1, false, false);
                check(!s.pending(), "answered -> frames accepted");
                check(!r.has("force landscape") && !r.has("force portrait") && !s.refused(), "no giving in");
                s.timeout(1);
                check(!r.has("fullframe") && !s.refused(), "a late timeout of an answered request is ignored");
            }
        });

        test("turned back and forth quickly: the answer of the older request is stale", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);                                   // #1 portrait
                s.onSize(LW, LH);                                   // #2 landscape again
                check(r.has("orient 1872x1404#2"), "second request sent: " + r.log);
                s.onGeom(PW, PH, 1, false, false);                  // the PC answers the first one (it was already switching)
                check(s.pending() && !r.has("force landscape") && !r.has("force portrait"), "stale answer ignored, still waiting for #2");
                s.onGeom(LW, LH, 2, false, false);
                check(!s.pending() && !s.refused(), "answer #2 accepted");
            }
        });

        test("same size again (landscape <-> reverse landscape) is not a change", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(LW, LH);
                s.onSize(LW, LH);
                check(r.log.isEmpty() && !s.pending(), "nothing happens");
            }
        });

        test("the PC refuses (cannot rotate): the activity is turned back and locked, a full picture is requested afterwards", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.onGeom(LW, LH, 1, true, false);                   // refused: the PC keeps 1872x1404
                check(r.has("force landscape") && s.refused(), "forced back to landscape: " + r.log);
                check(!s.pending(), "no longer pending (the PC's frames have the shape we are going back to)");
                r.clear();
                s.onSize(LW, LH);                                   // the activity finished turning
                check(r.has("fullframe") && !r.has("orient 1872x1404#2"), "full picture requested, no new T_ORIENT: " + r.log);
                r.clear();
                s.onSize(PW, PH);                                   // the sensor / the system turns it again
                check(r.has("force landscape") && !r.has("orient 1404x1872#2"), "locked: put back instead of asking again: " + r.log);
            }
        });

        test("the PC answers with a different size than asked (without the refusal flag)", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.onGeom(LW, LH, 1, false, false);
                check(r.has("force landscape") && s.refused(), "mismatch handled like a refusal: " + r.log);
            }
        });

        test("an old PC never answers: timeout turns the activity back and asks for the picture", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.timeout(1);
                check(r.has("force landscape") && s.refused() && !s.pending(), "gave in: " + r.log);
                r.clear();
                s.onSize(LW, LH);
                check(r.has("fullframe"), "picture requested once the size matches the PC again: " + r.log);
            }
        });

        test("timeout after the surface already went back to the PC's size", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.onSize(LW, LH);                                   // #2, back at the PC's size
                s.timeout(1);                                       // the first one: stale, ignored
                check(!r.has("fullframe") && s.pending(), "stale timeout ignored: " + r.log);
                s.timeout(2);
                check(r.has("fullframe") && !r.has("force landscape") && s.refused() && !s.pending(), "nothing to turn, just ask for the picture: " + r.log);
            }
        });

        test("announcement after the tablet was already turned (session started while portrait)", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);                           // HELLO said landscape (the display activity was not up yet)
                s.onSize(PW, PH);                                   // #1, sent before the announcement arrived
                s.onGeom(LW, LH, 0, false, false);                  // the announcement: must not start a second request while #1 is out
                check(r.log.size() == 2 && s.pending(), "only one request: " + r.log);
                s.onGeom(PW, PH, 1, false, false);
                check(!s.pending(), "answered");
            }
        });

        test("announcement of another size than ours (the PC kept an older monitor): ask for ours", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(PW, PH, 0, false, false);
                check(r.has("orient 1872x1404#1") && s.pending(), "asked for our size: " + r.log);
            }
        });

        test("a session that ends clears the waiting state", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.sessionEnded();
                check(!s.pending(), "not pending after the session ended");
                s.timeout(1);
                s.onGeom(PW, PH, 1, false, false);
                check(r.log.size() == 2, "late events of the dead session do nothing: " + r.log);
            }
        });

        // ---- NO_ROTATE (T_GEOM flags bit1): the PC does not follow the tablet's orientation right now ----------------------------------------

        test("NO_ROTATE announcement at the start of a session: locked to the PC's shape at once, also when the tablet already has it", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, true);
                check(r.is("force landscape"), "locked to landscape, nothing asked: " + r.log);
                check(s.refused() && !s.pending(), "locked, not waiting");
                r.clear();
                s.onSize(PW, PH);                                   // the system turns the surface anyway
                check(r.is("force landscape"), "the sensor is not followed: put back, no T_ORIENT: " + r.log);
                check(!s.pending(), "frames of the PC's shape are not held back");
                r.clear();
                s.onSize(LW, LH);
                check(r.is("fullframe"), "back at the PC's shape: the wiped shadow copy needs a full picture: " + r.log);

                Rec r2 = new Rec();
                OrientSync s2 = new OrientSync(r2);
                s2.sessionStarted(PW, PH);                          // a PC with a portrait desktop
                s2.onGeom(PW, PH, 0, false, true);
                check(r2.is("force portrait") && s2.refused(), "locked to portrait: " + r2.log);
            }
        });

        test("NO_ROTATE announcement with another shape than the tablet's: turn to the PC's shape, then ask for the picture", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(PW, PH);                           // the tablet is held in portrait, the PC's desktop is landscape and stays
                s.onGeom(LW, LH, 0, false, true);
                check(r.is("force landscape") && s.refused() && !s.pending(), "turned to landscape, no T_ORIENT: " + r.log);
                r.clear();
                s.onSize(LW, LH);                                   // the activity finished turning
                check(r.is("fullframe"), "full picture requested once the shape matches: " + r.log);
            }
        });

        test("NO_ROTATE announcement while a request is pending: the request is moot", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);                                   // #1 asks for portrait
                check(s.pending() && r.has("orient 1404x1872#1"), "request out: " + r.log);
                r.clear();
                s.onGeom(LW, LH, 0, false, true);                   // the PC's rotation was switched off meanwhile
                check(r.is("force landscape") && s.refused() && !s.pending(), "request cancelled, locked to landscape: " + r.log);
                r.clear();
                s.timeout(1);                                       // the deadline of the moot request passes
                check(r.log.isEmpty() && s.refused(), "timeout ignored: " + r.log);
                s.onGeom(LW, LH, 1, true, true);                    // the late refusal of that request: the same lock again, nothing new
                check(r.is("force landscape") && s.refused() && !s.pending(), "late refusal is harmless: " + r.log);
                r.clear();
                s.onSize(LW, LH);
                check(r.is("fullframe"), "turned back: full picture requested: " + r.log);
            }
        });

        test("a repeated NO_ROTATE announcement is harmless", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, true);
                s.onGeom(LW, LH, 0, false, true);
                check(r.is("force landscape", "force landscape") && s.refused() && !s.pending(), "no requests, still locked: " + r.log);
            }
        });

        test("a later announcement without NO_ROTATE releases the lock (the tablet already has the PC's shape: nothing is asked)", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, true);                   // locked to landscape
                r.clear();
                s.onGeom(LW, LH, 0, false, false);                  // the PC's switch is on again
                check(r.is("release") && !s.refused() && !s.pending(), "released, nothing asked: " + r.log);
                r.clear();
                s.onSize(PW, PH);                                   // the activity follows the sensor again
                check(r.is("orient 1404x1872#1", "arm#1") && s.pending(), "turning is negotiated again: " + r.log);
                s.onGeom(PW, PH, 1, false, false);
                check(!s.pending() && !s.refused() && r.log.size() == 2, "answered, not locked: " + r.log);
            }
        });

        test("unlock while the tablet is in the other shape: release, then the tablet's shape is requested", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(PW, PH);                           // held in portrait
                s.onGeom(LW, LH, 0, false, true);                   // locked to the PC's landscape; the activity has not turned yet
                r.clear();
                s.onGeom(LW, LH, 0, false, false);                  // the PC follows again before it did
                check(r.is("release", "orient 1404x1872#1", "arm#1") && s.pending() && !s.refused(), "released and asked for the tablet's shape: " + r.log);
                check(!r.has("fullframe"), "no full picture requested: the PC rebuilds its picture for the request");
                r.clear();
                s.onGeom(PW, PH, 1, false, false);
                check(r.log.isEmpty() && !s.pending() && !s.refused(), "answered, follows the sensor: " + r.log);
            }
        });

        test("unlock when the shadow copy was wiped and the surface has the PC's shape: full picture, no request", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.onGeom(LW, LH, 1, true, false);                   // refused: locked to landscape, the surface is still portrait, needs a picture later
                r.clear();
                s.onGeom(PW, PH, 0, false, false);                  // the PC's desktop is portrait now and it follows again
                check(r.is("release", "fullframe") && !s.pending() && !s.refused(), "released, full picture requested, nothing asked: " + r.log);
                r.clear();
                s.onSize(LW, LH);                                   // the sensor now moves it to landscape: normal negotiation
                check(r.is("orient 1872x1404#2", "arm#2"), "follows the sensor: " + r.log);
            }
        });

        test("no full picture is left over after an unlock that asked for the tablet's shape", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(PW, PH);
                s.onGeom(LW, LH, 0, false, true);                   // locked, shadow copy owes a picture
                s.onGeom(LW, LH, 0, false, false);                  // unlocked: #1 asks for portrait
                s.onGeom(PW, PH, 1, false, false);                  // the PC delivered it (and a full picture with it)
                s.onGeom(PW, PH, 0, false, true);                   // locked again (same shape)
                r.clear();
                s.onGeom(PW, PH, 0, false, false);                  // and free again
                check(r.is("release"), "just the release, no stale full-picture request: " + r.log);
            }
        });

        test("the PC's switch flipped back and forth in one session", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, true);                   // off: locked to landscape
                s.onGeom(LW, LH, 0, false, false);                  // on: released
                s.onSize(PW, PH);                                   // the tablet is turned to portrait: #1
                s.onGeom(PW, PH, 1, false, false);                  // the PC follows
                check(!s.refused() && !s.pending(), "following: " + r.log);
                r.clear();
                s.onGeom(PW, PH, 0, false, true);                   // off again: locked to the PC's current (portrait) shape
                check(r.is("force portrait") && s.refused(), "locked to portrait: " + r.log);
                r.clear();
                s.onSize(LW, LH);                                   // the system turns the surface anyway
                check(r.is("force portrait"), "put back, nothing asked: " + r.log);
            }
        });

        test("an answer with NO_ROTATE locks even when the shape matches", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);                                   // #1 asks for portrait
                r.clear();
                s.onGeom(PW, PH, 1, false, true);                   // the PC did it, but it will not follow any further
                check(r.is("force portrait") && s.refused() && !s.pending(), "locked to portrait: " + r.log);
                r.clear();
                s.onSize(LW, LH);                                   // the system turns the surface away
                check(r.is("force portrait"), "the sensor does not drive any more requests: " + r.log);
                r.clear();
                s.onSize(PW, PH);
                check(r.is("fullframe"), "back at the PC's shape: full picture: " + r.log);
            }
        });

        test("an answer with NO_ROTATE and another shape than asked is handled like a refusal", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                r.clear();
                s.onGeom(LW, LH, 1, false, true);
                check(r.is("force landscape") && s.refused() && !s.pending(), "turned back: " + r.log);
                r.clear();
                s.onSize(LW, LH);
                check(r.is("fullframe"), "full picture afterwards: " + r.log);
            }
        });

        test("a stale answer (older request) with NO_ROTATE is ignored", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);                                   // #1
                s.onSize(LW, LH);                                   // #2
                r.clear();
                s.onGeom(PW, PH, 1, false, true);
                check(r.log.isEmpty() && s.pending() && !s.refused(), "ignored: " + r.log);
            }
        });

        test("locked: the system turns the surface away and back (twice): a full picture every time", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.sessionStarted(LW, LH);
                s.onSize(PW, PH);
                s.onGeom(LW, LH, 1, true, false);                   // refused
                s.onSize(LW, LH);                                   // turned back: full picture #1
                r.clear();
                s.onSize(PW, PH);                                   // the system turns it away again (the shadow copy is wiped again) ...
                check(r.is("force landscape"), "put back: " + r.log);
                r.clear();
                s.onSize(LW, LH);                                   // ... and back: the PC knows nothing about the wipe
                check(r.is("fullframe"), "full picture again: " + r.log);
            }
        });

        test("NO_ROTATE / releases without a running session, and in a fresh session, do nothing", new Runnable() {
            public void run() {
                Rec r = new Rec();
                OrientSync s = new OrientSync(r);
                s.onGeom(LW, LH, 0, false, true);                   // no session yet
                check(r.log.isEmpty() && !s.refused(), "ignored before the session: " + r.log);
                s.sessionStarted(LW, LH);
                s.onGeom(LW, LH, 0, false, true);                   // locked
                s.sessionEnded();
                r.clear();
                s.onGeom(LW, LH, 0, false, false);                  // late frame of the dead session
                check(r.log.isEmpty(), "ignored after the session: " + r.log);
                s.sessionStarted(LW, LH);                           // the reconnect: a new session starts unlocked
                check(!s.refused() && !s.pending(), "fresh session is not locked");
                s.onGeom(LW, LH, 0, false, false);
                check(r.log.isEmpty(), "nothing to release in a fresh session: " + r.log);
            }
        });

        System.out.println(checks + " checks, " + failures + " failures");
        System.exit(failures == 0 ? 0 : 1);
    }
}
