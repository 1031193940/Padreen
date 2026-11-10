package com.padreen.app;

import android.os.Handler;
import android.view.MotionEvent;
import android.view.MotionEvent.P;

import java.util.ArrayList;
import java.util.List;

/** JVM test of the touch gate's state machine (stubs for Handler / MotionEvent; run with android/app/test/run-tests.ps1). */
public class TouchGateTest {
    static int failures = 0;

    static final class Rec implements TouchGate.Sink {
        final List<String> log = new ArrayList<String>();

        @Override
        public void touch(MotionEvent e) {
            String n;
            switch (e.getActionMasked()) {
                case 0: n = "DOWN"; break;
                case 1: n = "UP"; break;
                case 2: n = "MOVE"; break;
                case 3: n = "CANCEL"; break;
                case 5: n = "PDOWN"; break;
                case 6: n = "PUP"; break;
                default: n = "?";
            }
            log.add(n + "@" + Handler.now + "(ev " + e.getEventTime() + ")");
        }

        @Override
        public void cancelAll() {
            log.add("cancelAll");
        }

        @Override
        public void refresh() {
            log.add("REFRESH");
        }

        @Override
        public void swipe(int kind) {
            log.add(kind == TouchGate.SWIPE_UP ? "SWIPE_UP" : "SWIPE_DOWN");
        }

        boolean has(String prefix) {
            for (String s : log) if (s.startsWith(prefix)) return true;
            return false;
        }

        int count(String prefix) {
            int n = 0;
            for (String s : log) if (s.startsWith(prefix)) n++;
            return n;
        }

        boolean anyTouch() {
            return has("DOWN") || has("UP") || has("MOVE") || has("CANCEL") || has("PDOWN") || has("PUP");
        }
    }

    /** Builds Android-style event streams for a set of fingers. */
    static final class Sim {
        final Rec rec = new Rec();
        final TouchGate gate = new TouchGate(rec);
        final List<P> active = new ArrayList<P>();

        Sim() {
            Handler.reset();
        }

        void feed(MotionEvent ev) {
            Handler.advanceTo(ev.getEventTime());           // flush timers that were due before this event
            gate.onTouch(ev);
            ev.recycle();
        }

        void down(long t, int id, float x, float y) {
            down(t, id, x, y, MotionEvent.TOOL_TYPE_FINGER);
        }

        void down(long t, int id, float x, float y, int tool) {
            active.add(new P(id, x, y, tool));
            int idx = active.size() - 1;
            feed(MotionEvent.make(active.size() == 1 ? MotionEvent.ACTION_DOWN : MotionEvent.ACTION_POINTER_DOWN, idx, t, active.toArray(new P[0])));
        }

        void move(long t, int id, float x, float y) {
            for (int i = 0; i < active.size(); i++) if (active.get(i).id == id) active.set(i, new P(id, x, y, active.get(i).tool));
            feed(MotionEvent.make(MotionEvent.ACTION_MOVE, 0, t, active.toArray(new P[0])));
        }

        /** all fingers move by (dx, dy) from where they are */
        void moveAll(long t, float dx, float dy) {
            for (int i = 0; i < active.size(); i++) {
                P p = active.get(i);
                active.set(i, new P(p.id, p.x + dx, p.y + dy, p.tool));
            }
            feed(MotionEvent.make(MotionEvent.ACTION_MOVE, 0, t, active.toArray(new P[0])));
        }

        void up(long t, int id) {
            int idx = -1;
            for (int i = 0; i < active.size(); i++) if (active.get(i).id == id) idx = i;
            feed(MotionEvent.make(active.size() == 1 ? MotionEvent.ACTION_UP : MotionEvent.ACTION_POINTER_UP, idx, t, active.toArray(new P[0])));
            active.remove(idx);
        }

        void cancel(long t) {
            feed(MotionEvent.make(MotionEvent.ACTION_CANCEL, 0, t, active.toArray(new P[0])));
            active.clear();
        }

        void end() {
            Handler.advanceTo(Handler.now + 5000);
        }
    }

    static void check(String name, boolean ok, Sim s) {
        if (!ok) {
            failures++;
            System.out.println("FAIL  " + name + "   log=" + s.rec.log);
        } else {
            System.out.println("ok    " + name + "   log=" + s.rec.log);
        }
    }

    public static void main(String[] a) {
        // 1. a single tap: down + up inside the hold -> both are delivered at the UP (no extra delay beyond the tap itself)
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500);
            s.up(30, 0);
            s.end();
            check("single tap passes through", s.rec.log.size() == 2 && s.rec.log.get(0).startsWith("DOWN@30") && s.rec.log.get(1).startsWith("UP@30") && !s.rec.has("REFRESH"), s);
        }
        // 2. a drag: the down is delayed by at most HOLD_FIRST_MS, later moves stream through at once
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500);
            for (int t = 10; t <= 200; t += 10) s.move(t, 0, 500 + t, 500);
            s.up(210, 0);
            s.end();
            boolean down60 = s.rec.log.get(0).startsWith("DOWN@60");
            boolean late = s.rec.log.get(s.rec.log.size() - 2).startsWith("MOVE@200");
            check("drag: down released at 60 ms, then streaming", down60 && late && s.rec.count("MOVE") == 20 && s.rec.has("UP@210"), s);
        }
        // 3. three-finger tap, fingers land within 30 ms: the PC sees nothing, refresh fires once
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(15, 1, 600, 620);
            s.down(30, 2, 800, 600);
            s.up(150, 0);
            s.up(160, 1);
            s.up(170, 2);
            s.end();
            check("three-finger tap -> refresh only, nothing sent to the PC", !s.rec.anyTouch() && s.rec.count("REFRESH") == 1 && !s.rec.has("SWIPE") && !s.rec.has("cancelAll"), s);
        }
        // 4. sloppy tap: second finger at 50 ms, third at 100 ms (inside the 140 ms group window)
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(50, 1, 600, 620);
            s.down(100, 2, 800, 600);
            s.up(250, 0);
            s.up(255, 1);
            s.up(260, 2);
            s.end();
            check("sloppy three-finger tap (3rd finger at 100 ms) still recognised", !s.rec.anyTouch() && s.rec.count("REFRESH") == 1, s);
        }
        // 5. too late: third finger at 200 ms -> ordinary multi-touch, no refresh
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(70, 1, 600, 620);
            s.down(200, 2, 800, 600);
            s.up(300, 0);
            s.up(305, 1);
            s.up(310, 2);
            s.end();
            check("third finger too late -> plain multi-touch, no refresh", s.rec.anyTouch() && !s.rec.has("REFRESH"), s);
        }
        // 6. swipe up
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 900);
            s.down(10, 1, 600, 900);
            s.down(20, 2, 800, 900);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 0, -30);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("three-finger swipe up -> SWIPE_UP once, no touches, no refresh", !s.rec.anyTouch() && s.rec.count("SWIPE_UP") == 1 && !s.rec.has("REFRESH") && !s.rec.has("SWIPE_DOWN"), s);
        }
        // 7. swipe down
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 300);
            s.down(10, 1, 600, 300);
            s.down(20, 2, 800, 300);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 0, 30);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("three-finger swipe down -> SWIPE_DOWN once, no touches, no refresh", !s.rec.anyTouch() && s.rec.count("SWIPE_DOWN") == 1 && !s.rec.has("REFRESH") && !s.rec.has("SWIPE_UP"), s);
        }
        // 8. horizontal three-finger swipe: nothing at all (the PC sees nothing either)
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 40, 0);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("horizontal three-finger swipe -> nothing", !s.rec.anyTouch() && s.rec.log.isEmpty(), s);
        }
        // 8b. ... also to the left
        {
            Sim s = new Sim();
            s.down(0, 0, 1200, 600);
            s.down(10, 1, 1400, 600);
            s.down(20, 2, 1600, 600);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, -40, 0);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("horizontal three-finger swipe to the left -> nothing", !s.rec.anyTouch() && s.rec.log.isEmpty(), s);
        }
        // 8c. a long sideways drag is still nothing
        {
            Sim s = new Sim();
            s.down(0, 0, 200, 600);
            s.down(10, 1, 400, 600);
            s.down(20, 2, 600, 600);
            for (int t = 40; t <= 1200; t += 40) s.moveAll(t, 30, 0);
            s.up(1220, 0);
            s.up(1225, 1);
            s.up(1230, 2);
            s.end();
            check("a long horizontal three-finger drag -> nothing", s.rec.log.isEmpty(), s);
        }
        // 8d. diagonal: neither direction dominates -> nothing at all (and it is not a tap either)
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 30, 30);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("diagonal three-finger drag -> nothing", s.rec.log.isEmpty(), s);
        }
        // 8e. too short: 150 px sideways is neither a tap (moved more than the slop) nor a swipe
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            for (int t = 40; t <= 200; t += 40) s.moveAll(t, 30, 0);
            s.up(300, 0);
            s.up(305, 1);
            s.up(310, 2);
            s.end();
            check("short horizontal three-finger movement (150 px) -> nothing", s.rec.log.isEmpty(), s);
        }
        // 8f. a mostly vertical swipe with some sideways drift stays a vertical swipe
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 900);
            s.down(10, 1, 600, 900);
            s.down(20, 2, 800, 900);
            for (int t = 40; t <= 280; t += 40) s.moveAll(t, 10, -40);
            s.up(300, 0);
            s.up(305, 1);
            s.up(310, 2);
            s.end();
            check("vertical swipe with a little drift -> SWIPE_UP", s.rec.log.size() == 1 && s.rec.count("SWIPE_UP") == 1, s);
        }
        // 8g. ... and a mostly horizontal one with some vertical drift is still nothing
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 900);
            s.down(10, 1, 600, 900);
            s.down(20, 2, 800, 900);
            for (int t = 40; t <= 280; t += 40) s.moveAll(t, 40, 10);
            s.up(300, 0);
            s.up(305, 1);
            s.up(310, 2);
            s.end();
            check("horizontal swipe with a little vertical drift -> nothing", s.rec.log.isEmpty(), s);
        }
        // 8h. a fourth finger during a vertical swipe spoils it
        {
            Sim s = new Sim();
            s.down(0, 0, 300, 900);
            s.down(10, 1, 500, 900);
            s.down(20, 2, 700, 900);
            for (int t = 40; t <= 120; t += 40) s.moveAll(t, 0, -30);
            s.down(140, 3, 900, 900);
            for (int t = 160; t <= 500; t += 40) s.moveAll(t, 0, -40);
            s.up(520, 3);
            s.up(525, 0);
            s.up(530, 1);
            s.up(535, 2);
            s.end();
            check("a fourth finger spoils the swipe -> nothing", s.rec.log.isEmpty(), s);
        }
        // 8i. four fingers landing together and swiping sideways: not our gesture (the fourth finger spoils it)
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            s.down(30, 3, 1000, 600);
            for (int t = 60; t <= 400; t += 40) s.moveAll(t, 40, 0);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.up(435, 3);
            s.end();
            check("four fingers swiping sideways -> no gesture", !s.rec.has("SWIPE") && !s.rec.has("REFRESH"), s);
        }
        // 8j. palm-like contacts (a few mm apart) sliding sideways: no gesture, plain touches
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500);
            s.down(10, 1, 530, 520);
            s.down(20, 2, 560, 505);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 40, 0);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("palm-like contacts sliding sideways -> no gesture, plain touches", !s.rec.has("SWIPE") && !s.rec.has("REFRESH") && s.rec.anyTouch(), s);
        }
        // 8k. a resting thumb and two fingers that land later swiping sideways: no gesture
        {
            Sim s = new Sim();
            s.down(0, 0, 100, 700);
            s.down(500, 1, 600, 600);
            s.down(520, 2, 800, 600);
            for (int t = 560; t <= 960; t += 40) s.moveAll(t, 40, 0);
            s.up(1000, 1);
            s.up(1010, 2);
            s.up(1100, 0);
            s.end();
            check("resting thumb + two fingers swiping sideways -> no gesture", !s.rec.has("SWIPE") && !s.rec.has("REFRESH") && s.rec.anyTouch(), s);
        }
        // 8l. a single finger swiping sideways is an ordinary drag for the PC
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 40, 0);
            s.up(420, 0);
            s.end();
            check("one finger dragging sideways -> plain touch events", s.rec.count("DOWN") == 1 && s.rec.count("MOVE") == 10 && s.rec.count("UP") == 1, s);
        }
        // 9. a thumb that has been resting, then two fingers land together later: normal multi-touch, no gesture
        {
            Sim s = new Sim();
            s.down(0, 0, 100, 700);          // resting thumb
            s.down(500, 1, 600, 600);
            s.down(520, 2, 800, 600);
            s.up(620, 1);
            s.up(630, 2);
            s.up(900, 0);
            s.end();
            check("resting thumb + two fingers later -> no gesture", s.rec.anyTouch() && !s.rec.has("REFRESH") && !s.rec.has("SWIPE"), s);
        }
        // 10. a two-finger tap (right-click on the PC): delivered in order once the touch is over
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500);
            s.down(20, 1, 700, 500);
            s.up(100, 1);
            s.up(110, 0);
            s.end();
            check("two-finger tap delivered in order", s.rec.count("DOWN") == 1 && s.rec.count("PDOWN") == 1 && s.rec.count("PUP") == 1 && s.rec.count("UP") == 1 && !s.rec.has("REFRESH"), s);
        }
        // 11. stylus: no delay, no gestures
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500, MotionEvent.TOOL_TYPE_STYLUS);
            s.move(10, 0, 520, 500);
            s.up(20, 0);
            s.end();
            check("stylus is forwarded immediately", s.rec.log.size() == 3 && s.rec.log.get(0).startsWith("DOWN@0"), s);
        }
        // 12. four fingers: not a gesture
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            s.down(30, 3, 1000, 600);
            s.up(150, 0);
            s.up(155, 1);
            s.up(160, 2);
            s.up(165, 3);
            s.end();
            check("four fingers -> no refresh", !s.rec.has("REFRESH") && !s.rec.has("SWIPE"), s);
        }
        // 13. three-finger long press: not a tap
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            s.up(1500, 0);
            s.up(1505, 1);
            s.up(1510, 2);
            s.end();
            check("three-finger long press -> nothing", s.rec.log.isEmpty(), s);
        }
        // 14. cancel during the hold: nothing was sent, nothing to undo
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.cancel(20);
            s.end();
            check("cancel while holding -> nothing sent", s.rec.log.isEmpty(), s);
        }
        // 15. gate disabled: pass-through with no delay
        {
            Sim s = new Sim();
            s.gate.setEnabled(false);
            s.down(0, 0, 400, 600);
            s.down(15, 1, 600, 620);
            s.down(30, 2, 800, 600);
            s.up(150, 0);
            s.up(160, 1);
            s.up(170, 2);
            s.end();
            check("gate disabled -> everything passes through at once", s.rec.count("DOWN@0") == 1 && !s.rec.has("REFRESH") && s.rec.anyTouch(), s);
        }
        // 15b. gate disabled: even a three-finger sideways swipe is just plain touch
        {
            Sim s = new Sim();
            s.gate.setEnabled(false);
            s.down(0, 0, 400, 600);
            s.down(10, 1, 600, 600);
            s.down(20, 2, 800, 600);
            for (int t = 40; t <= 400; t += 40) s.moveAll(t, 40, 0);
            s.up(420, 0);
            s.up(425, 1);
            s.up(430, 2);
            s.end();
            check("gate disabled -> a sideways three-finger swipe is just touch", s.rec.count("MOVE") == 10 && s.rec.anyTouch() && !s.rec.has("SWIPE") && !s.rec.has("REFRESH"), s);
        }
        // 16. a swipe that happens after a tap-like landing, then a second three-finger tap right after (separate touches)
        {
            Sim s = new Sim();
            s.down(0, 0, 400, 600);
            s.down(15, 1, 600, 620);
            s.down(30, 2, 800, 600);
            s.up(120, 0);
            s.up(125, 1);
            s.up(130, 2);
            s.down(1000, 0, 400, 600);
            s.down(1010, 1, 600, 620);
            s.down(1020, 2, 800, 600);
            s.up(1100, 0);
            s.up(1105, 1);
            s.up(1110, 2);
            s.end();
            check("two separate three-finger taps -> two refresh requests", s.rec.count("REFRESH") == 2 && !s.rec.anyTouch(), s);
        }
        // 17. a palm breaking into three contacts a few mm apart: not three fingertips
        {
            Sim s = new Sim();
            s.down(0, 0, 500, 500);
            s.down(10, 1, 530, 520);
            s.down(20, 2, 560, 505);
            s.up(150, 0);
            s.up(155, 1);
            s.up(160, 2);
            s.end();
            check("palm-like contacts (a few mm apart) -> no gesture", !s.rec.has("REFRESH") && !s.rec.has("SWIPE") && s.rec.anyTouch(), s);
        }
        // 18. contacts of two hands, 20 cm apart: not one hand
        {
            Sim s = new Sim();
            s.down(0, 0, 200, 500);
            s.down(10, 1, 400, 500);
            s.down(20, 2, 1800, 500);
            s.up(150, 0);
            s.up(155, 1);
            s.up(160, 2);
            s.end();
            check("two hands (far apart) -> no gesture", !s.rec.has("REFRESH") && !s.rec.has("SWIPE") && s.rec.anyTouch(), s);
        }
        if (MotionEvent.live != 0) {
            failures++;
            System.out.println("FAIL  leaked MotionEvent copies: " + MotionEvent.live);
        }
        System.out.println(failures == 0 ? "ALL OK" : (failures + " FAILURE(S)"));
        System.exit(failures == 0 ? 0 : 1);
    }
}
