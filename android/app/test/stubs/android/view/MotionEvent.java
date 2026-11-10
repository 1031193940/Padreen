package android.view;

import java.util.ArrayList;

/** Test stub of MotionEvent: just enough for TouchGate. */
public final class MotionEvent {
    public static final int ACTION_MASK = 0xff, ACTION_DOWN = 0, ACTION_UP = 1, ACTION_MOVE = 2, ACTION_CANCEL = 3, ACTION_POINTER_DOWN = 5, ACTION_POINTER_UP = 6;
    public static final int TOOL_TYPE_FINGER = 1, TOOL_TYPE_STYLUS = 2;

    public static final class P {
        public final int id;
        public final float x, y;
        public final int tool;

        public P(int id, float x, float y, int tool) {
            this.id = id;
            this.x = x;
            this.y = y;
            this.tool = tool;
        }
    }

    private int action;                 // masked action | (index << 8)
    private long time;
    private ArrayList<P> ptrs = new ArrayList<P>();
    public static int live;             // obtained and not yet recycled (leak check)

    public static MotionEvent make(int action, int index, long time, P... p) {
        MotionEvent e = new MotionEvent();
        e.action = action | (index << 8);
        e.time = time;
        for (P q : p) e.ptrs.add(q);
        live++;
        return e;
    }

    public static MotionEvent obtain(MotionEvent o) {
        MotionEvent e = new MotionEvent();
        e.action = o.action;
        e.time = o.time;
        e.ptrs = new ArrayList<P>(o.ptrs);
        live++;
        return e;
    }

    public void recycle() {
        live--;
    }

    public int getActionMasked() { return action & ACTION_MASK; }
    public int getActionIndex() { return (action & 0xff00) >> 8; }
    public int getPointerCount() { return ptrs.size(); }
    public int getPointerId(int i) { return ptrs.get(i).id; }
    public float getX(int i) { return ptrs.get(i).x; }
    public float getY(int i) { return ptrs.get(i).y; }
    public int getToolType(int i) { return ptrs.get(i).tool; }
    public long getEventTime() { return time; }
}
