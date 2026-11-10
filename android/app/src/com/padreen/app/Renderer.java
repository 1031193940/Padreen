package com.padreen.app;

import android.util.Log;

import java.util.ArrayList;

/**
 * Decouples the network thread from the (slow, throttled) e-ink pipeline.
 *
 * The network thread only copies decoded gray tiles into {@link #shadow} and widens a pending rectangle; this thread posts the newest
 * content of that rectangle to the display, at most once per {@code minGapNs}.  While SurfaceFlinger / the T1000 are busy (one real
 * panel update every ~0.6 s) new tiles keep accumulating in the shadow buffer, so nothing stale is ever queued: the next post always
 * carries the latest pixels of everything that changed in between.
 *
 * Pending work is kept in THREE classes that are never merged with each other (an earlier version took the bounding box of everything,
 * so one gray settle box plus a pointer update on the other side of the screen became one huge GL16 post - a "full-screen refresh" nobody
 * asked for):
 *   - fast  : the fast 1-bit modes (DU / A2): one union rectangle, posted first (latency matters most);
 *   - gray  : GL16 settle boxes: one group per box (boxes are merged only when they touch);
 *   - full  : a GC16 full refresh (manual): when posted it covers (and drops) the pending parts inside it.
 */
final class Renderer implements Runnable {
    static final String TAG = "Padreen";

    /** Called by the render thread after a batch was posted, once per merged UPDATE. */
    interface AckSink {
        void ack(int seq, long tHdr, long tBody, long tDecoded, long tDrawn, long tPosted);
    }

    /** The e-ink surface of the visible display activity; null while the app is in the background (then nothing is drawn, the shadow keeps filling). */
    private volatile Eink.Fb fb;
    /** Logical size of the display (portrait or landscape: the device can be turned); changed only by {@link #resize}, under {@link #lock}. */
    private volatile int W, H;
    /** True once a frame of the current session arrived (an attach then repaints the shadow; before that the waiting message stays). */
    private volatile boolean hasContent;
    private byte[] shadow;                      // guarded by lock; replaced by resize()
    /** Bumped by every resize: a post that was prepared for the old shape must not reach the (re-allocated) surface. */
    private volatile int epoch;
    private final Object lock = new Object();
    private final long minGapNs;
    private volatile AckSink sink;
    private volatile boolean running = true;
    /** Debug: when >= 0, every post uses this raw EinkMode value instead of the protocol mode's waveform. */
    static volatile int rawWave = -1;
    /** Debug: log every post with its nanoTime so logcat lines (SurfaceFlinger stages) can be aligned with the PC clock. */
    static volatile boolean trace;

    /** A pending rectangle with the protocol mode to post it in and the ACKs to send once it was posted. */
    private static final class Group {
        int x0 = Integer.MAX_VALUE, y0 = Integer.MAX_VALUE, x1 = -1, y1 = -1;
        int mode = -1;
        ArrayList<long[]> acks = new ArrayList<long[]>();

        boolean empty() {
            return x1 < 0;
        }

        void add(int x, int y, int w, int h, int m, long[] ack) {
            if (x < x0) x0 = x;
            if (y < y0) y0 = y;
            if (x + w > x1) x1 = x + w;
            if (y + h > y1) y1 = y + h;
            mode = m;
            if (ack != null) acks.add(ack);
        }

        boolean touches(int x, int y, int w, int h) {
            return !(x > x1 || x + w < x0 || y > y1 || y + h < y0);
        }

        boolean inside(Group o) {      // this lies completely inside o
            return x0 >= o.x0 && y0 >= o.y0 && x1 <= o.x1 && y1 <= o.y1;
        }

        void clear() {
            x0 = y0 = Integer.MAX_VALUE;
            x1 = y1 = -1;
            mode = -1;
            acks = new ArrayList<long[]>();
        }
    }

    // pending state, guarded by lock
    private final Group fast = new Group();
    private final Group full = new Group();
    private final ArrayList<Group> grays = new ArrayList<Group>();

    Renderer(int w, int h, long minGapMs) {
        this.W = w;
        this.H = h;
        this.shadow = new byte[w * h];
        java.util.Arrays.fill(shadow, (byte) 0xFF);
        this.minGapNs = minGapMs * 1000000L;
    }

    /**
     * The display activity came to the front (or was created): draw on its surface from now on, and repaint the whole panel from the shadow
     * buffer in one GC16 refresh - the panel showed another app meanwhile.
     */
    void attach(Eink.Fb f) {
        synchronized (lock) {
            boolean already = fb == f;                 // surfaceChanged fires again for the same surface: nothing was overwritten, no repaint
            fb = f;
            if (hasContent && !already) {
                Log.i(TAG, "display back in front: repainting the whole panel from the shadow buffer (GC16)");
                full.add(0, 0, W, H, 3, null);
            }
            lock.notifyAll();
        }
    }

    /** The surface goes away (activity stopped / destroyed): stop drawing; waits for a post that is in flight. */
    void detach(Eink.Fb f) {
        synchronized (lock) {
            if (fb == f) fb = null;
        }
        if (f != null) synchronized (f) {
            // a post that started before the detach finishes before we return
        }
    }

    boolean hasContent() {
        return hasContent;
    }

    /** The session is over: forget the picture (the next one starts from white) so a stale frame is never repainted over a message. */
    void resetContent() {
        synchronized (lock) {
            hasContent = false;
            java.util.Arrays.fill(shadow, (byte) 0xFF);
            fast.clear();
            full.clear();
            grays.clear();
        }
    }

    int width() {
        return W;
    }

    int height() {
        return H;
    }

    /**
     * The device was turned: the display now has another shape (1872x1404 <-> 1404x1872).  The shadow copy of the panel no longer fits and is
     * dropped (the PC delivers a new picture of the new size); until it does, a plain white page is shown.  No-op if the size is the same.
     */
    void resize(int w, int h) {
        synchronized (lock) {
            if (w == W && h == H) return;
            Log.i(TAG, "display size " + W + "x" + H + " -> " + w + "x" + h);
            W = w;
            H = h;
            shadow = new byte[w * h];
            java.util.Arrays.fill(shadow, (byte) 0xFF);
            fast.clear();
            full.clear();
            grays.clear();
            hasContent = false;
            epoch++;
            fast.add(0, 0, w, h, 6, null);          // blank page (A2): whatever the system left on the panel while it turned is wiped
            lock.notifyAll();
        }
    }

    void setSink(AckSink s) {
        sink = s;
    }

    void stop() {
        running = false;
        synchronized (lock) {
            lock.notifyAll();
        }
    }

    /** Protocol mode -> class: 3 = full refresh, 2 = gray settle, 1 = the fast modes. */
    private static int prio(int mode) {
        switch (mode) {
            case 3:
                return 3;
            case 2:
                return 2;
            default:
                return 1;
        }
    }

    /** Protocol mode -> waveform id understood by EPDCDevice.setForceNextPostMode. */
    static int waveform(int mode) {
        if (rawWave >= 0) return rawWave;
        switch (mode) {
            case 1:
                return Eink.MODE_DU;
            case 2:
                return Eink.MODE_GL16;
            case 3:
                return Eink.MODE_GC16_FULL;
            case 4:
                return Eink.MODE_DU4_5BIT;
            case 5:
                return Eink.MODE_A2;
            case 6:
                return Eink.MODE_A2_F6;
            case 7:
                return Eink.MODE_A2_F7;
            case 8:
                return Eink.MODE_A2_F8;
            case 9:
                return Eink.MODE_A2_F9;
            default:
                return Eink.MODE_DU4_4BIT;
        }
    }

    /** Network thread: store the decoded tile (w*h gray bytes at src[off]) and mark it pending. */
    void submit(int mode, int x, int y, int w, int h, byte[] src, int off, int seq, long tHdr, long tBody, long tDec) {
        synchronized (lock) {
            if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > W || y + h > H) return;     // a frame of the old shape that was still on the wire
            hasContent = true;
            for (int r = 0; r < h; r++) System.arraycopy(src, off + r * w, shadow, (y + r) * W + x, w);
            long[] ack = new long[]{seq, tHdr, tBody, tDec};
            int p = prio(mode);
            if (p == 1) {
                fast.add(x, y, w, h, mode, ack);
            } else if (p == 3) {
                full.add(x, y, w, h, mode, ack);
            } else {
                Group g = null;
                for (Group q : grays)
                    if (q.touches(x, y, w, h)) {
                        g = q;
                        break;
                    }
                if (g == null) {
                    g = new Group();
                    grays.add(g);
                }
                g.add(x, y, w, h, mode, ack);
            }
            lock.notifyAll();
        }
    }

    /** Manual full refresh from the tablet itself (volume key / gesture): the whole shadow buffer once, in GC16. */
    void requestFull() {
        synchronized (lock) {
            full.add(0, 0, W, H, 3, null);
            lock.notifyAll();
        }
    }

    // ---- post statistics: one line every 5 s while something is being drawn (what reached the panel and how big it was)
    private long stPosts, stBig, stFull, stArea, stSlow;
    private long stSince = System.nanoTime();

    private void account(int mode, long area, int x, int y, int w, int h) {
        long panel = (long) W * H;
        stPosts++;
        stArea += area;
        if (area * 2 >= panel) stBig++;
        if (area * 10 >= panel * 9) stFull++;
        int p = prio(mode);
        if (p != 1) {                                   // gray / full-panel waveforms are the ones that flash: always log them
            stSlow++;
            Log.i(TAG, "slow post: protocol mode " + mode + " rect " + x + "," + y + " " + w + "x" + h + " (" + (area * 100 / panel) + "% of the panel)");
        }
        long now = System.nanoTime();
        if (now - stSince >= 5000000000L) {
            Log.i(TAG, "render: " + stPosts + " posts in " + (now - stSince) / 1000000 + " ms, avg " + (stArea * 100 / Math.max(1, stPosts * panel)) + "% of the panel, "
                    + stBig + " >=50%, " + stFull + " >=90%, " + stSlow + " gray/full");
            stPosts = stBig = stFull = stArea = stSlow = 0;
            stSince = now;
        }
    }

    private boolean pending() {
        return !fast.empty() || !full.empty() || !grays.isEmpty();
    }

    @Override
    public void run() {
        byte[] tmp = new byte[W * H];
        long lastPost = 0;
        long batches = 0, tiles = 0;
        while (running) {
            synchronized (lock) {
                while (running && (!pending() || fb == null)) {          // nothing to do, or nowhere to draw (app in the background)
                    try {
                        lock.wait();
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
            if (!running) break;
            // pace: updates that arrive while we wait simply widen the pending rectangles
            long wait = lastPost + minGapNs - System.nanoTime();
            if (wait > 0) {
                try {
                    Thread.sleep(wait / 1000000L, (int) (wait % 1000000L));
                } catch (InterruptedException e) {
                    return;
                }
            }
            int x0, y0, x1, y1, mode, jobEpoch, jobW, jobH;
            ArrayList<long[]> acks;
            synchronized (lock) {
                if (!pending()) continue;               // everything was dropped while we paced (the device was turned)
                Group job = new Group();
                if (!full.empty()) {
                    // a full refresh shows everything pending inside it anyway
                    job.add(full.x0, full.y0, full.x1 - full.x0, full.y1 - full.y0, full.mode, null);
                    job.acks.addAll(full.acks);
                    full.clear();
                    if (!fast.empty() && fast.inside(job)) {
                        job.acks.addAll(fast.acks);
                        fast.clear();
                    }
                    for (int i = grays.size() - 1; i >= 0; i--)
                        if (grays.get(i).inside(job)) {
                            job.acks.addAll(grays.get(i).acks);
                            grays.remove(i);
                        }
                } else if (!fast.empty()) {
                    job.add(fast.x0, fast.y0, fast.x1 - fast.x0, fast.y1 - fast.y0, fast.mode, null);
                    job.acks.addAll(fast.acks);
                    fast.clear();
                } else {
                    Group g = grays.remove(0);
                    job.add(g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0, g.mode, null);
                    job.acks.addAll(g.acks);
                }
                x0 = job.x0;
                y0 = job.y0;
                x1 = job.x1;
                y1 = job.y1;
                mode = job.mode;
                acks = job.acks;
                jobEpoch = epoch;
                jobW = W;
                jobH = H;
                int w = x1 - x0;
                if (tmp.length < w * (y1 - y0)) tmp = new byte[w * (y1 - y0)];
                for (int r = 0; r < y1 - y0; r++) System.arraycopy(shadow, (y0 + r) * W + x0, tmp, r * w, w);
            }
            final int w = x1 - x0, h = y1 - y0;
            final Eink.Fb f = fb;
            if (f == null) continue;                  // detached while we were pacing: the next attach repaints everything
            try {
                long tDrawn, tPosted;
                synchronized (f) {
                    // the device was turned meanwhile (the picture has the old shape), or the surface's buffers do not have the shape of this
                    // picture (yet / any more): drop it - the PC delivers a new picture of the new shape
                    if (jobEpoch != epoch || f.width != jobW || f.height != jobH) continue;
                    f.blitGray(tmp, 0, x0, y0, w, h);
                    tDrawn = System.nanoTime();
                    f.post(x0, y0, w, h, waveform(mode));
                    tPosted = System.nanoTime();
                }
                lastPost = tPosted;
                account(mode, (long) w * h, x0, y0, w, h);
                if (trace) Log.i(TAG, "POSTED mode=" + mode + " rect=" + x0 + "," + y0 + " " + w + "x" + h + " merged=" + acks.size() + " nano=" + tPosted);
                batches++;
                tiles += acks.size();
                AckSink s = sink;
                if (s != null) {
                    for (long[] a : acks) s.ack((int) a[0], a[1], a[2], a[3], tDrawn, tPosted);
                }
            } catch (Throwable t) {
                Log.e(TAG, "render failed", t);
                f.abortDraw();                        // a canvas that was locked but never posted would block every later frame
            }
        }
        Log.i(TAG, "renderer stopped, batches=" + batches + " merged updates=" + tiles);
    }
}
