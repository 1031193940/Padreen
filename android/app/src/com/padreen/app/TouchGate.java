package com.padreen.app;

import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.MotionEvent;

import java.util.ArrayList;

/**
 * Sits between the touch screen and the PC and recognises the tablet's own three-finger gestures:
 *   - three-finger TAP        = refresh the whole screen now
 *   - three-finger SWIPE UP   = multiple desktops (Win+Tab on the PC)
 *   - three-finger SWIPE DOWN = window switcher (Alt+Tab on the PC)
 * A recognised gesture must not reach the PC at all (no stray taps, no right-click menu).  Windows turns two quick fingers into a
 * right-click, so the first fingers of a possible gesture are held back for a moment: if the third finger lands in time
 * (HOLD_GROUP_MS after the first one) everything held is thrown away; otherwise it is released in order and later events stream
 * through at once.  A lone finger waits at most HOLD_FIRST_MS; stylus input and a disabled gate (Settings) are never delayed.
 *
 * What counts as the gesture: exactly three fingers that all landed inside the hold window, i.e. a deliberate three-finger touch -
 * not a thumb that has been resting on the screen for a while while two other fingers pinch.  All on the UI thread.
 */
final class TouchGate {
    static final long HOLD_FIRST_MS = 60;       // a lone first finger is held this long
    static final long HOLD_GROUP_MS = 140;      // ... and the group of fingers has until this long after the first finger to complete
    static final long TAP_MAX_MS = 600;         // a tap is over this soon
    static final float TAP_SLOP_PX = 70f;       // ... and no finger moved more than this (about 8 mm at 227 dpi)
    static final float SWIPE_PX = 200f;         // vertical travel of the three fingers that makes a swipe (about 22 mm)
    static final float SWIPE_DOMINANCE = 1.6f;  // ... and it must be this many times the horizontal travel (a diagonal drag is nothing)
    static final float MIN_SPREAD_PX = 90f;     // fingertips are at least ~10 mm apart; closer contacts are a palm / knuckle breaking into pieces
    static final float MAX_SPREAD_PX = 1100f;   // ... and the three fingers belong to one hand (about 12 cm at most)
    static final int SWIPE_UP = 1, SWIPE_DOWN = 2;
    static final String TAG = "Padreen";

    interface Sink {
        void touch(MotionEvent e);              // forward a touch event to the PC

        void cancelAll();                       // lift every finger the PC still holds down

        void refresh();                         // three-finger tap

        void swipe(int kind);                   // three-finger swipe (SWIPE_UP / SWIPE_DOWN)
    }

    private enum State { IDLE, HOLD, FORWARD, GESTURE }

    private final Sink sink;
    private final Handler handler = new Handler(Looper.getMainLooper());
    private State state = State.IDLE;
    private boolean enabled = true;

    private final ArrayList<MotionEvent> held = new ArrayList<MotionEvent>();
    private long t0;                            // event time of the first finger
    private int fingers;                        // fingers that went down during the hold
    private final float[] startX = new float[16], startY = new float[16];

    // gesture tracking
    private int gestureIds;                     // bit mask of the pointer ids of the three fingers
    private float maxMove;
    private long landed;                    // ms between the first and the third finger
    private boolean fired, spoiled;

    private final Runnable flushTask = new Runnable() {
        @Override
        public void run() {
            if (state == State.HOLD) release();
        }
    };

    TouchGate(Sink sink) {
        this.sink = sink;
    }

    void setEnabled(boolean on) {
        if (!on) reset();
        enabled = on;
    }

    /** The surface went away / the activity stopped: drop what is held, lift fingers the PC still holds. */
    void reset() {
        handler.removeCallbacks(flushTask);
        for (MotionEvent m : held) m.recycle();
        held.clear();
        if (state == State.FORWARD) sink.cancelAll();
        state = State.IDLE;
    }

    void onTouch(MotionEvent ev) {
        int a = ev.getActionMasked();
        if (!enabled) {
            sink.touch(ev);
            return;
        }
        switch (state) {
            case IDLE:
                if (a != MotionEvent.ACTION_DOWN) {                       // we missed the start (activity just came up): pass it on
                    sink.touch(ev);
                    return;
                }
                if (ev.getToolType(0) != MotionEvent.TOOL_TYPE_FINGER) {  // stylus / mouse: no gestures, no delay
                    state = State.FORWARD;
                    sink.touch(ev);
                    return;
                }
                state = State.HOLD;
                t0 = ev.getEventTime();
                fingers = 1;
                remember(ev, 0);
                hold(ev);
                handler.postAtTime(flushTask, t0 + HOLD_FIRST_MS);
                return;
            case HOLD:
                if (a == MotionEvent.ACTION_CANCEL) {                    // nothing was sent: nothing to undo
                    drop();
                    return;
                }
                hold(ev);
                if (a == MotionEvent.ACTION_POINTER_DOWN) {
                    fingers++;
                    remember(ev, ev.getActionIndex());
                    if (fingers >= 3) {
                        if (fingers == 3 && ev.getPointerCount() == 3 && plausibleHand(ev)) startGesture(ev);
                        else release();                                    // four or more fingers / a palm / two hands: not our gesture, ordinary multi-touch
                    } else {
                        handler.removeCallbacks(flushTask);                // second finger: the third one may still come
                        handler.postAtTime(flushTask, t0 + HOLD_GROUP_MS);
                    }
                } else if (a == MotionEvent.ACTION_UP) {                   // the whole touch is over inside the hold (a quick tap)
                    release();
                }
                return;
            case FORWARD:
                sink.touch(ev);
                if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) state = State.IDLE;
                return;
            case GESTURE:
                track(ev);
                if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
                    if (a == MotionEvent.ACTION_UP && !fired && !spoiled && ev.getEventTime() - t0 <= TAP_MAX_MS && maxMove <= TAP_SLOP_PX) {
                        Log.i(TAG, "three-finger tap: landed within " + landed + " ms, lasted " + (ev.getEventTime() - t0) + " ms, moved at most " + (int) maxMove + " px");
                        sink.refresh();
                    }
                    state = State.IDLE;
                }
                return;
        }
    }

    private void remember(MotionEvent ev, int index) {
        int id = ev.getPointerId(index) & 15;
        startX[id] = ev.getX(index);
        startY[id] = ev.getY(index);
    }

    private void hold(MotionEvent ev) {
        held.add(MotionEvent.obtain(ev));
    }

    private void drop() {
        handler.removeCallbacks(flushTask);
        for (MotionEvent m : held) m.recycle();
        held.clear();
        state = State.IDLE;
    }

    /** Not a gesture (in time): send what was held, in order; from now on events stream through. */
    private void release() {
        handler.removeCallbacks(flushTask);
        boolean over = false;
        for (MotionEvent m : held) {
            int a = m.getActionMasked();
            sink.touch(m);
            if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) over = true;
            m.recycle();
        }
        held.clear();
        state = over ? State.IDLE : State.FORWARD;
    }

    /** Three separate fingertips of one hand: pairwise distances between MIN_SPREAD_PX and MAX_SPREAD_PX. */
    private boolean plausibleHand(MotionEvent ev) {
        float lo = Float.MAX_VALUE, hi = 0;
        for (int i = 0; i < 3; i++)
            for (int j = i + 1; j < 3; j++) {
                float d = (float) Math.hypot(ev.getX(i) - ev.getX(j), ev.getY(i) - ev.getY(j));
                lo = Math.min(lo, d);
                hi = Math.max(hi, d);
            }
        boolean ok = lo >= MIN_SPREAD_PX && hi <= MAX_SPREAD_PX;
        if (!ok) Log.i(TAG, "three contacts ignored as a gesture: spread " + (int) lo + ".." + (int) hi + " px (not three fingertips of one hand)");
        return ok;
    }

    private void startGesture(MotionEvent ev) {
        handler.removeCallbacks(flushTask);
        for (MotionEvent m : held) m.recycle();                            // the PC never sees any of it
        held.clear();
        state = State.GESTURE;
        landed = ev.getEventTime() - t0;
        gestureIds = 0;
        for (int i = 0; i < ev.getPointerCount(); i++) gestureIds |= 1 << (ev.getPointerId(i) & 15);
        maxMove = 0;
        fired = false;
        spoiled = false;
    }

    private void track(MotionEvent ev) {
        int a = ev.getActionMasked();
        if (ev.getPointerCount() > 3) spoiled = true;                      // a fourth finger: this is something else
        int lifting = a == MotionEvent.ACTION_POINTER_UP ? ev.getActionIndex() : -1;
        float sx = 0, sy = 0;
        int n = 0;
        for (int i = 0; i < ev.getPointerCount(); i++) {
            int id = ev.getPointerId(i) & 15;
            if ((gestureIds & (1 << id)) == 0) {
                spoiled = true;
                continue;
            }
            float dx = ev.getX(i) - startX[id], dy = ev.getY(i) - startY[id];
            maxMove = Math.max(maxMove, (float) Math.hypot(dx, dy));
            if (i == lifting) continue;
            sx += dx;
            sy += dy;
            n++;
        }
        if (n == 0 || fired || spoiled || a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) return;
        float dx = sx / n, dy = sy / n;
        if (Math.abs(dy) >= SWIPE_PX && Math.abs(dy) >= SWIPE_DOMINANCE * Math.abs(dx)) {
            fired = true;
            Log.i(TAG, "three-finger swipe " + (dy < 0 ? "up" : "down") + ": landed within " + landed + " ms, travelled " + (int) dy + " / " + (int) dx + " px");
            sink.swipe(dy < 0 ? SWIPE_UP : SWIPE_DOWN);
        }
    }
}
