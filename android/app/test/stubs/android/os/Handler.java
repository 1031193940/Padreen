package android.os;

import java.util.ArrayList;
import java.util.Iterator;

/** Test stub: a Handler with a virtual clock (postAtTime / removeCallbacks / advanceTo). */
public class Handler {
    private static final ArrayList<Object[]> queue = new ArrayList<Object[]>();   // {Long when, Runnable r}
    public static long now;

    public Handler(Looper l) {
    }

    public boolean postAtTime(Runnable r, long when) {
        queue.add(new Object[]{when, r});
        return true;
    }

    public void removeCallbacks(Runnable r) {
        for (Iterator<Object[]> it = queue.iterator(); it.hasNext(); ) if (it.next()[1] == r) it.remove();
    }

    /** Run every task that is due up to `t` (in time order), then move the clock. */
    public static void advanceTo(long t) {
        while (true) {
            Object[] best = null;
            for (Object[] o : queue) if ((Long) o[0] <= t && (best == null || (Long) o[0] < (Long) best[0])) best = o;
            if (best == null) break;
            queue.remove(best);
            now = (Long) best[0];
            ((Runnable) best[1]).run();
        }
        now = t;
    }

    public static void reset() {
        queue.clear();
        now = 0;
    }
}
