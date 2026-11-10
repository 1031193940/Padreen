package com.padreen.app;

/**
 * The tablet's side of "which size is the picture?" while the device is turned (T_ORIENT / T_GEOM, see protocol/PROTOCOL.md), kept free of
 * any Android class so it can be unit-tested on the JVM.
 *
 * The tablet owns the orientation (the activity follows the sensor); the PC owns the virtual monitor.  When the surface of the display
 * activity gets another size (portrait <-> landscape) the tablet
 *   1. wipes its shadow copy of the panel (it has the wrong shape now) and asks the PC for a desktop of the new size (T_ORIENT, numbered);
 *   2. ignores every UPDATE / touch until the PC answers with the same number (T_GEOM): frames that were already on the wire still have the
 *      old shape, and touches sent now would be mapped onto the old monitor.
 * The PC answers with the size it really delivers.  If that is not the size of the surface (the PC cannot or may not rotate, or an old PC
 * never answers: timeout), the tablet gives in: it turns the activity into the orientation of the PC's picture and stops following the sensor
 * until the PC follows again (or the session ends).  Whenever the shadow copy was wiped without the PC rebuilding its picture, the tablet
 * asks for a full one.
 *
 * The PC can also say so in advance: a T_GEOM announcement (seq 0) with the NO_ROTATE flag means "this PC does not follow the tablet's
 * orientation right now" (its user switched automatic rotation off) - the tablet locks to the announced shape at once instead of finding out
 * by asking.  A later announcement without the flag (the switch was turned on again) releases the lock: the activity follows the sensor
 * again, and if the tablet is held in the other shape the PC is asked for it.
 */
final class OrientSync {
    /** What the state machine needs from its owner.  Called with this object's lock held - keep the implementations short and non-blocking. */
    interface Sink {
        void sendOrient(int w, int h, int seq);      // T_ORIENT: please deliver a w x h desktop
        void forceOrientation(boolean landscape);    // turn the activity to this orientation and keep it there (the PC cannot follow)
        void releaseOrientation();                   // stop forcing an orientation: the activity follows the sensor again
        void armTimeout(int seq);                    // call timeout(seq) after the reply deadline
        void requestFullFrame();                     // the shadow copy was wiped but the PC may not know: ask for a full picture
    }

    private final Sink sink;
    private int w, h;                  // logical size of the display surface (what the tablet shows)
    private int pcW, pcH;              // size of the picture the PC delivers (last confirmed)
    private int seq;                   // number of the latest request (1..255); 0 = none yet
    private boolean pending;           // a request is out and its answer is not here yet
    private boolean active;            // a session is running
    private boolean refused;           // the PC does not follow: the orientation is locked to the PC's picture
    private boolean needFull;          // the orientation was forced back; once the surface has the PC's size again the wiped shadow needs a full picture

    OrientSync(Sink sink) {
        this.sink = sink;
    }

    /** A session started: the HELLO told the PC this size, so that is what it delivers. */
    synchronized void sessionStarted(int w, int h) {
        this.w = w;
        this.h = h;
        pcW = w;
        pcH = h;
        seq = 0;
        pending = false;
        refused = false;
        needFull = false;
        active = true;
    }

    synchronized void sessionEnded() {
        active = false;
        pending = false;
        needFull = false;
    }

    /** True while UPDATEs / touches must be ignored (the PC's picture and ours have different shapes). */
    synchronized boolean pending() {
        return pending;
    }

    /** True while the PC does not follow the tablet's orientation: the activity must stay in the orientation of the PC's picture. */
    synchronized boolean refused() {
        return refused;
    }

    /** The display surface now has this logical size (called for every surfaceChanged; only a real change counts). */
    synchronized void onSize(int nw, int nh) {
        if (nw == w && nh == h) return;
        w = nw;
        h = nh;
        if (!active) {
            pending = false;
            return;
        }
        if (refused) {
            if (nw == pcW && nh == pcH) {
                pending = false;
                if (needFull) {                      // the shadow copy was wiped on the way and the PC may not have sent a picture since
                    needFull = false;
                    sink.requestFullFrame();
                }
            } else {
                needFull = true;                     // the surface left the PC's shape, so its shadow copy was wiped: it needs a picture on the way back
                sink.forceOrientation(pcW >= pcH);   // still not the PC's shape (the system turned us): put it back
            }
            return;
        }
        request();
    }

    /**
     * T_GEOM from the PC: seq 0 = an announcement (start of a session, after a reconnect, or the PC's rotation switch was flipped),
     * otherwise the answer to request `gseq`.  `refusal` = flag bit0 (answers: the PC could not switch), `noRotate` = flag bit1 (the PC keeps
     * the shape it delivers, whatever the tablet does, until it announces otherwise).
     */
    synchronized void onGeom(int gw, int gh, int gseq, boolean refusal, boolean noRotate) {
        if (!active) return;
        if (gseq == 0) {
            pcW = gw;
            pcH = gh;
            if (noRotate) {                                      // the PC keeps this shape: turn to it and stay there
                refused = true;
                pending = false;                                 // an open request is moot (whatever it was answered with)
                if (gw != w || gh != h) needFull = true;         // the surface is about to turn to the PC's shape: its wiped shadow needs a picture then
                sink.forceOrientation(gw >= gh);                 // idempotent: a repeated announcement does no harm
                return;
            }
            if (refused) {                                       // the PC follows the sensor again: unlock
                refused = false;
                sink.releaseOrientation();
            }
            if (needFull && w == gw && h == gh) {                // the surface has the PC's shape already, but its shadow copy was wiped meanwhile
                needFull = false;
                sink.requestFullFrame();
            }
            if (!pending && (gw != w || gh != h)) request();     // the tablet is turned relative to the PC's desktop (before the announcement arrived / while locked)
            return;
        }
        if (gseq != seq) return;                                 // the answer to a request that was overtaken by a newer one
        pending = false;
        pcW = gw;
        pcH = gh;
        if (refusal || gw != w || gh != h) {
            refused = true;
            needFull = true;                                     // the PC's first frame may arrive before the activity has turned: ask again afterwards
            sink.forceOrientation(gw >= gh);
        } else if (noRotate) {                                   // the PC delivers what we asked for, but it will not follow any further: stop driving it
            refused = true;
            sink.forceOrientation(gw >= gh);
        }
    }

    /** The reply deadline of request `tseq` passed (an old PC never answers). */
    synchronized void timeout(int tseq) {
        if (!active || !pending || tseq != seq) return;
        pending = false;
        refused = true;
        if (w == pcW && h == pcH) {
            sink.requestFullFrame();                             // back at the PC's size: the shadow was wiped on the way, the PC never noticed
        } else {
            needFull = true;
            sink.forceOrientation(pcW >= pcH);
        }
    }

    private void request() {
        seq = seq % 255 + 1;
        pending = true;
        needFull = false;                  // the PC rebuilds its picture for every request (a refusal / timeout sets the flag again)
        sink.sendOrient(w, h, seq);
        sink.armTimeout(seq);
    }
}
