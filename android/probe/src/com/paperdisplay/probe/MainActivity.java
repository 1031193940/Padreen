package com.paperdisplay.probe;

import android.app.Activity;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ActivityInfo;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/**
 * Phase-0 probe. Driven from the PC:
 *   adb shell am broadcast -a com.paperdisplay.probe.CMD -p com.paperdisplay.probe --es c "info; fill c=255 m=98 via=addepdc"
 * Commands are separated by ';', arguments are key=value. Everything is logged under tag PaperProbe.
 *
 * Paths under test:
 *   plain  : normal SurfaceView, mode chosen via=addepdc | next | force | none
 *   hw     : android.eink.view.HWSurfaceView + postFrameBuffer (pen fast path)
 * This probe never calls setEpdcDefaultMode / setDelayRefreshInterval / setNeedCompareFrame etc.
 */
public class MainActivity extends Activity {
    static final String TAG = "PaperProbe";
    static final String ACTION = "com.paperdisplay.probe.CMD";

    private FrameLayout root;
    private SurfaceView curSv;
    private HwPath hw;
    private volatile boolean ready;
    private volatile String kind = "none";
    private HandlerThread ht;
    private Handler wh;
    private final Paint paint = new Paint();
    private volatile Map<String, String> curArgs = new HashMap<String, String>();

    static void log(String s) {
        Log.i(TAG, s);
    }

    static double ms(long a, long b) {
        return (b - a) / 1e6;
    }

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context ctx, Intent i) {
            final String c = i.getStringExtra("c");
            if (c == null) return;
            wh.post(new Runnable() {
                public void run() {
                    runLine(c);
                }
            });
        }
    };

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);
        setContentView(root);
        hideSystemUi();
        ht = new HandlerThread("probe-worker");
        ht.start();
        wh = new Handler(ht.getLooper());
        log("Eink.init: " + Eink.init());
        registerReceiver(receiver, new IntentFilter(ACTION));
        wh.post(new Runnable() {
            public void run() {
                useSurface("plain");
            }
        });
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    @Override
    protected void onDestroy() {
        try {
            unregisterReceiver(receiver);
        } catch (Throwable ignored) {
        }
        ht.quitSafely();
        super.onDestroy();
    }

    private void hideSystemUi() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }

    // ------------------------------------------------------------------ helpers

    private void ui(final Runnable r) {
        final CountDownLatch l = new CountDownLatch(1);
        runOnUiThread(new Runnable() {
            public void run() {
                try {
                    r.run();
                } finally {
                    l.countDown();
                }
            }
        });
        try {
            l.await(5, TimeUnit.SECONDS);
        } catch (InterruptedException ignored) {
        }
    }

    private void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException ignored) {
        }
    }

    private boolean waitReady(long timeoutMs) {
        long end = System.currentTimeMillis() + timeoutMs;
        while (!ready && System.currentTimeMillis() < end) sleep(20);
        return ready;
    }

    private static int gi(Map<String, String> a, String k, int def) {
        String v = a.get(k);
        if (v == null) return def;
        try {
            return Integer.decode(v);
        } catch (Throwable t) {
            return def;
        }
    }

    private static String hex(Object o) {
        return o == null ? "null" : (o instanceof Integer ? "0x" + Integer.toHexString((Integer) o) : String.valueOf(o));
    }

    // ------------------------------------------------------------------ surface management

    private void useSurface(final String k) {
        ready = false;
        ui(new Runnable() {
            public void run() {
                if (curSv != null) {
                    root.removeView(curSv);
                    curSv = null;
                }
                SurfaceView sv;
                if (k.equals("hw")) {
                    hw = new HwPath();
                    log("HwPath.create: " + hw.create(MainActivity.this));
                    if (hw.view == null) return;
                    sv = (SurfaceView) hw.view;
                } else {
                    sv = new SurfaceView(MainActivity.this);
                }
                sv.getHolder().setFormat(PixelFormat.RGBA_8888);
                sv.getHolder().addCallback(new SurfaceHolder.Callback() {
                    public void surfaceCreated(SurfaceHolder h) {
                        log("surfaceCreated");
                    }

                    public void surfaceChanged(SurfaceHolder h, int f, int w, int hh) {
                        log("surfaceChanged " + w + "x" + hh + " fmt=" + f);
                        ready = true;
                    }

                    public void surfaceDestroyed(SurfaceHolder h) {
                        log("surfaceDestroyed");
                        ready = false;
                    }
                });
                root.addView(sv, new FrameLayout.LayoutParams(-1, -1));
                curSv = sv;
                kind = k;
            }
        });
        log("useSurface " + k + " ready=" + waitReady(3000));
    }

    private void setOri(String v) {
        final int o;
        if ("land".equals(v)) o = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
        else if ("rland".equals(v)) o = ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE;
        else if ("port".equals(v)) o = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
        else if ("rport".equals(v)) o = ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT;
        else o = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED;
        runOnUiThread(new Runnable() {
            public void run() {
                setRequestedOrientation(o);
            }
        });
        sleep(1800);
        // If the orientation did not change there is no surface callback: fall back to Surface.isValid()
        if (!ready && curSv != null && curSv.getHolder().getSurface().isValid()) ready = true;
        log("setOri " + v + " ready=" + waitReady(3000) + " surface="
                + (curSv == null ? "null" : curSv.getWidth() + "x" + curSv.getHeight()));
    }

    // ------------------------------------------------------------------ commands

    private void runLine(String line) {
        for (String cmd : line.split(";")) {
            cmd = cmd.trim();
            if (!cmd.isEmpty()) runCmd(cmd);
        }
    }

    private void runCmd(String cmd) {
        String[] tok = cmd.split("\\s+");
        String name = tok[0];
        Map<String, String> a = new HashMap<String, String>();
        for (int i = 1; i < tok.length; i++) {
            int e = tok[i].indexOf('=');
            if (e > 0) a.put(tok[i].substring(0, e), tok[i].substring(e + 1));
        }
        long t0 = System.nanoTime();
        curArgs = a;
        log(">> " + cmd);
        try {
            if (name.equals("info")) {
                info();
            } else if (name.equals("penmode")) {
                if (hw != null) hw.setPenPostMode(gi(a, "m", 1));
                log("setAhwPenPostMode " + gi(a, "m", 1));
            } else if (name.equals("autohw")) {
                if (hw != null) hw.setAutoHandWrite(gi(a, "v", 0) == 1);
                log("setAutoHandWrite " + gi(a, "v", 0));
            } else if (name.equals("watch")) {
                watchPower(gi(a, "ms", 5000));
            } else if (name.equals("burst")) {
                burst(a);
            } else if (name.equals("shapes")) {
                shapes(a);
            } else if (name.equals("fbtest")) {
                fbtest(a);
            } else if (name.equals("fbtest2")) {
                fbtest2(a);
            } else if (name.equals("fbclear")) {
                fbclear(a);
            } else if (name.equals("fbtest3")) {
                fbtest3(a);
            } else if (name.equals("rows2")) {
                rows2(a);
            } else if (name.equals("hb")) {
                heartbeat(a);
            } else if (name.equals("ori")) {
                setOri(a.get("v"));
            } else if (name.equals("use")) {
                useSurface(a.get("v"));
            } else if (name.equals("sleep")) {
                sleep(gi(a, "ms", 100));
            } else if (name.equals("api")) {
                Eink.dumpAccess(a.get("c"));
            } else if (name.equals("refresh")) {
                Eink.call(Eink.forceRefresh, gi(a, "m", 98));
            } else if (name.equals("fill") || name.equals("rect") || name.equals("blink")) {
                if (!waitReady(3000)) {
                    log("surface not ready");
                } else {
                    int w = curSv.getWidth(), h = curSv.getHeight();
                    Rect r = name.equals("fill")
                            ? new Rect(0, 0, w, h)
                            : new Rect(gi(a, "l", 0), gi(a, "t", 0), gi(a, "r", w), gi(a, "b", h));
                    int mode = gi(a, "m", -1);
                    String via = a.containsKey("via") ? a.get("via") : "addepdc";
                    if (name.equals("blink")) {
                        int reps = gi(a, "reps", 4), gap = gi(a, "gap", 400);
                        for (int i = 0; i < reps; i++) {
                            draw(r, (i % 2 == 0) ? 0 : 255, mode, via);
                            sleep(gap);
                        }
                    } else {
                        draw(r, gi(a, "c", 255), mode, via);
                    }
                }
            } else {
                log("unknown command: " + name);
            }
        } catch (Throwable t) {
            Log.e(TAG, "cmd failed: " + cmd, t);
        }
        log(String.format(Locale.US, "<< %s %.1fms", name, ms(t0, System.nanoTime())));
    }

    private void info() {
        Display d = getWindowManager().getDefaultDisplay();
        DisplayMetrics dm = new DisplayMetrics();
        d.getRealMetrics(dm);
        log("display real=" + dm.widthPixels + "x" + dm.heightPixels + " dpi=" + dm.densityDpi
                + " rotation=" + d.getRotation() + " refreshRate=" + d.getRefreshRate());
        log("sdk=" + Build.VERSION.SDK_INT + " build=" + Build.DISPLAY + " model=" + Build.MODEL);
        log("surface=" + (curSv == null ? "null" : curSv.getWidth() + "x" + curSv.getHeight()) + " kind=" + kind + " ready=" + ready);
        log("EPDC defaultMode=" + hex(Eink.call(Eink.getDefaultMode)) + " lastMode=" + hex(Eink.call(Eink.getLastMode)));
        log("EPDC t1000Info=" + Eink.call(Eink.getT1000Info));
        log("EPDC powerStatus=" + hex(Eink.call(Eink.getPowerStatus)) + " pmic=" + hex(Eink.call(Eink.getPmicPowerStatus))
                + " needCompareFrame=" + Eink.call(Eink.getNeedCompare));
    }

    // ------------------------------------------------------------------ drawing

    private void draw(Rect r, int gray, int mode, String via) {
        if (kind.equals("hw")) hwDraw(r, gray, mode, via);
        else plainDraw(r, gray, mode, via);
    }

    /**
     * Six ways of putting a black 300x200 box on the panel through the HW surface. The human watching the
     * panel tells us which boxes really show up:
     *   A (100,100)  draw in active buffer, then postFrameBuffer, then unlockActiveBuffer   (what burst/hb did)
     *   B (700,100)  draw in active buffer, unlockActiveBuffer (queue to SF), THEN postFrameBuffer
     *   C (1300,100) draw in active buffer, unlockActiveBuffer, then fb.invalidate(rect, mode)
     *   D (100,800)  fb.clipRect + fb.drawColor(black) (draw straight into the FB), then postFrameBuffer
     *   E (700,800)  own GraphicBuffer filled black + fb.drawGraphic(...), then postFrameBuffer
     *   F (1300,800) draw in active buffer + unlockActiveBuffer only (no explicit post)
     */
    private void shapes(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("shapes needs the hw surface (use v=hw)");
            return;
        }
        int m = gi(a, "m", 1);
        boolean force = gi(a, "force", 1) == 1;
        log("createDrawApi: " + hw.createDrawApi());
        Object paintObj = hw.newPaint();
        Rect rA = new Rect(100, 100, 400, 300), rB = new Rect(700, 100, 1000, 300), rC = new Rect(1300, 100, 1600, 300);
        Rect rD = new Rect(100, 800, 400, 1000), rE = new Rect(700, 800, 1000, 1000), rF = new Rect(1300, 800, 1600, 1000);

        // A
        log("--- A: buffer canvas + postFrameBuffer + unlock");
        Object gb = hw.lockActiveBuffer();
        Canvas c = hw.lockCanvas(gb, new Rect(rA));
        paintBlack(c, rA);
        hw.unlockCanvasAndPost(gb, c);
        if (force) Eink.call(Eink.setForceNext, m);
        hw.postFrameBuffer(rA, m);
        hw.unlockActiveBuffer();
        sleep(1500);

        // B
        log("--- B: buffer canvas + unlock (queue), then postFrameBuffer");
        gb = hw.lockActiveBuffer();
        c = hw.lockCanvas(gb, new Rect(rB));
        paintBlack(c, rB);
        hw.unlockCanvasAndPost(gb, c);
        hw.unlockActiveBuffer();
        if (force) Eink.call(Eink.setForceNext, m);
        hw.postFrameBuffer(rB, m);
        sleep(1500);

        // C
        log("--- C: buffer canvas + unlock, then fb.invalidate");
        gb = hw.lockActiveBuffer();
        c = hw.lockCanvas(gb, new Rect(rC));
        paintBlack(c, rC);
        hw.unlockCanvasAndPost(gb, c);
        hw.unlockActiveBuffer();
        if (force) Eink.call(Eink.setForceNext, m);
        hw.invalidate(rC, m);
        sleep(1500);

        // D
        log("--- D: fb.clipRect + fb.drawColor, then postFrameBuffer");
        hw.fbCall(hw.fbSave);
        hw.fbCall(hw.fbClipRect, new Rect(rD));
        hw.fbCall(hw.fbDrawColor, Color.BLACK, android.graphics.PorterDuff.Mode.SRC, paintObj);
        hw.fbCall(hw.fbRestore);
        if (force) Eink.call(Eink.setForceNext, m);
        hw.postFrameBuffer(rD, m);
        sleep(1500);

        // E
        log("--- E: own GraphicBuffer + fb.drawGraphic, then postFrameBuffer");
        Object gb2 = hw.createGb(rE.width(), rE.height(), 0x33 | 0x100);
        log("  createGb -> " + gb2);
        if (gb2 != null) {
            Canvas c2 = hw.lockCanvas(gb2, null);
            if (c2 != null) {
                paintBlack(c2, new Rect(0, 0, rE.width(), rE.height()));
                hw.unlockCanvasAndPost(gb2, c2);
            }
            hw.fbCall(hw.fbDrawGraphic, gb2, (float) rE.left, (float) rE.top, paintObj);
            if (force) Eink.call(Eink.setForceNext, m);
            hw.postFrameBuffer(rE, m);
        }
        sleep(1500);

        // F
        log("--- F: buffer canvas + unlock only");
        gb = hw.lockActiveBuffer();
        c = hw.lockCanvas(gb, new Rect(rF));
        paintBlack(c, rF);
        hw.unlockCanvasAndPost(gb, c);
        hw.unlockActiveBuffer();
        sleep(2500);
        log("--- shapes done");
    }

    /** Mean luma (0..255) of the rect inside bmp. */
    private static double meanLuma(android.graphics.Bitmap bmp, Rect r) {
        long sum = 0;
        int n = 0;
        for (int y = r.top; y < r.bottom; y += 4) {
            for (int x = r.left; x < r.right; x += 4) {
                int p = bmp.getPixel(x, y);
                sum += ((p >> 16) & 0xff) * 3 + ((p >> 8) & 0xff) * 6 + (p & 0xff);
                n++;
            }
        }
        return n == 0 ? -1 : sum / 10.0 / n;
    }

    /** Put an opaque white full-screen picture into the FB and push it with a full-screen refresh. */
    private void fbclear(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("fbclear needs the hw surface");
            return;
        }
        hw.createDrawApi();
        int W = curSv.getWidth(), H = curSv.getHeight();
        Object pDef = hw.newPaint();
        int color = gi(a, "c", 255) == 0 ? Color.BLACK : Color.WHITE;
        fbFill(new Rect(0, 0, W, H), color, pDef);
        Eink.call(Eink.setForceNext, gi(a, "m", 98));
        hw.postFrameBuffer(new Rect(0, 0, W, H), gi(a, "m", 98));
        log("fbclear posted full screen");
    }

    /**
     * Scan GraphicBuffer sizes: fill with R=x, G=y (mod 256), drawGraphic 1:1 into the FB, read the FB back and
     * report what landed. Exact copy => R(dx)=dx, G(dy)=dy.
     */
    private void fbtest3(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("fbtest3 needs the hw surface");
            return;
        }
        hw.createDrawApi();
        int W = curSv.getWidth(), H = curSv.getHeight();
        Object pDef = hw.newPaint();
        int[][] sizes = {{200, 200}, {256, 256}, {400, 400}, {512, 512}, {640, 480}, {800, 600}, {1000, 700}, {1024, 1024},
                {1200, 900}, {1404, 1404}, {1872, 100}, {100, 1404}, {936, 702}, {1872, 1404}};
        int[] xs = {16, 64, 128, 200};
        for (int[] s : sizes) {
            int w = s[0], h = s[1];
            Object gb = hw.createGb(w, h, 0x33);
            if (gb == null) {
                log("size " + w + "x" + h + ": createGb null");
                continue;
            }
            int gw = -1, gh = -1, gf = -1;
            try {
                Class<?> cGb = gb.getClass();
                gw = (Integer) cGb.getMethod("getWidth").invoke(gb);
                gh = (Integer) cGb.getMethod("getHeight").invoke(gb);
                gf = (Integer) cGb.getMethod("getFormat").invoke(gb);
            } catch (Throwable t) {
                log("  getWidth failed: " + t);
            }
            int[] px = new int[w * h];
            for (int y = 0; y < h; y++) {
                int rowc = 0xFF000000 | ((y & 255) << 8);
                for (int x = 0; x < w; x++) px[y * w + x] = rowc | ((x & 255) << 16);
            }
            android.graphics.Bitmap bm = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
            bm.setPixels(px, 0, w, 0, 0, w, h);
            Canvas c = hw.lockCanvas(gb, null);
            c.drawBitmap(bm, new Rect(0, 0, w, h), new Rect(0, 0, w, h), null);
            hw.unlockCanvasAndPost(gb, c);
            fbFill(new Rect(0, 0, W, H), Color.WHITE, pDef);
            long t0 = System.nanoTime();
            hw.fbCall(hw.fbDrawGraphicRect, gb, new Rect(0, 0, w, h), new android.graphics.RectF(0, 0, w, h), pDef);
            double dt = ms(t0, System.nanoTime());
            android.graphics.Bitmap fbb = hw.captureFb(W, H);
            StringBuilder sb = new StringBuilder();
            sb.append(String.format(Locale.US, "size %4dx%-4d gb=%dx%d fmt=%d  draw=%.1fms  R@x=", w, h, gw, gh, gf, dt));
            boolean exact = true;
            for (int dx : xs) {
                if (dx >= w) continue;
                int r = (fbb.getPixel(dx, Math.min(5, h - 1)) >> 16) & 255;
                sb.append(r).append(r == (dx & 255) ? "" : "!").append(' ');
                if (r != (dx & 255)) exact = false;
            }
            sb.append(" G@y=");
            for (int dy : xs) {
                if (dy >= h) continue;
                int g = (fbb.getPixel(Math.min(5, w - 1), dy) >> 8) & 255;
                sb.append(g).append(g == (dy & 255) ? "" : "!").append(' ');
                if (g != (dy & 255)) exact = false;
            }
            int last = fbb.getPixel(w - 1, h - 1);
            int expR = (w - 1) & 255, expG = (h - 1) & 255;
            boolean lastOk = ((last >> 16) & 255) == expR && ((last >> 8) & 255) == expG;
            if (!lastOk) exact = false;
            sb.append(" last=(").append((last >> 16) & 255).append(',').append((last >> 8) & 255).append(") exp=(").append(expR).append(',').append(expG).append(")");
            sb.append(exact ? "   => EXACT" : "   => WRONG");
            log(sb.toString());
        }
        fbFill(new Rect(0, 0, W, H), Color.WHITE, pDef);
        log("--- fbtest3 done");
    }

    /** Four rows of ten 150x150 black squares drawn with FBHandSurface.drawGraphic + postFrameBuffer. */
    private void rows2(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("rows2 needs the hw surface");
            return;
        }
        hw.createDrawApi();
        Object pDef = hw.newPaint();
        int[] modes = {gi(a, "m1", 5), gi(a, "m2", 7), gi(a, "m3", 3), gi(a, "m4", 1)};
        int[] ys = {100, 400, 700, 1000};
        int sz = 150, pitch = 180, n = 10, gap = gi(a, "gap", 40), rowGap = gi(a, "rowgap", 3500);
        Object sq = hw.createGb(sz, sz, 0x33);
        Canvas c = hw.lockCanvas(sq, null);
        paintBlack(c, new Rect(0, 0, sz, sz));
        hw.unlockCanvasAndPost(sq, c);
        Rect src = new Rect(0, 0, sz, sz);
        for (int row = 0; row < modes.length; row++) {
            log("--- row " + row + " mode=" + modes[row]);
            long b0 = System.nanoTime();
            for (int i = 0; i < n; i++) {
                Rect dst = new Rect(40 + i * pitch, ys[row], 40 + i * pitch + sz, ys[row] + sz);
                long t0 = System.nanoTime();
                hw.fbCall(hw.fbDrawGraphicRect, sq, src, new android.graphics.RectF(dst), pDef);
                long t1 = System.nanoTime();
                Eink.call(Eink.setForceNext, modes[row]);
                hw.postFrameBuffer(dst, modes[row]);
                long t2 = System.nanoTime();
                log(String.format(Locale.US, "row %d #%d draw=%.2fms post=%.2fms (+%.0fms)", row, i, ms(t0, t1), ms(t1, t2), ms(b0, t0)));
                sleep(gap);
            }
            sleep(rowGap);
        }
        log("--- rows2 done");
    }

    /** Stricter FB test: before/after readback, gray fidelity of drawGraphic, and drawGraphic timing. */
    private void fbtest2(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("fbtest2 needs the hw surface");
            return;
        }
        log("createDrawApi: " + hw.createDrawApi());
        int W = curSv.getWidth(), H = curSv.getHeight();
        Object pDef = hw.newPaint();
        Rect r = new Rect(500, 500, 700, 700);

        // 1) strict before/after for the two drawGraphic variants and drawPath
        Object gb = hw.createGb(200, 200, 0x33);
        Canvas c = hw.lockCanvas(gb, null);
        paintBlack(c, new Rect(0, 0, 200, 200));
        hw.unlockCanvasAndPost(gb, c);
        for (int v = 0; v < 3; v++) {
            fbFill(r, Color.WHITE, pDef);
            double before = captureLuma(W, H, r);
            if (v == 0) hw.fbCall(hw.fbDrawGraphic, gb, (float) r.left, (float) r.top, pDef);
            else if (v == 1) hw.fbCall(hw.fbDrawGraphicRect, gb, new Rect(0, 0, 200, 200), new android.graphics.RectF(r), pDef);
            else {
                android.graphics.Path p = new android.graphics.Path();
                p.addRect(new android.graphics.RectF(r), android.graphics.Path.Direction.CW);
                hw.fbCall(hw.fbDrawPath, p, pDef);
            }
            double after = captureLuma(W, H, r);
            log(String.format(Locale.US, "variant %s: luma before=%.1f after=%.1f", new String[]{"drawGraphic(x,y)", "drawGraphic(rect)", "drawPath"}[v], before, after));
        }

        // 2) gray fidelity: 256-px-wide gradient, read back and compare
        Object gg = hw.createGb(256, 40, 0x33);
        Canvas cg = hw.lockCanvas(gg, null);
        for (int i = 0; i < 256; i++) {
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.rgb(i, i, i));
            cg.drawRect(i, 0, i + 1, 40, paint);
        }
        hw.unlockCanvasAndPost(gg, cg);
        Rect rg = new Rect(500, 800, 756, 840);
        fbFill(rg, Color.WHITE, pDef);
        hw.fbCall(hw.fbDrawGraphicRect, gg, new Rect(0, 0, 256, 40), new android.graphics.RectF(rg), pDef);
        android.graphics.Bitmap bm = hw.captureFb(W, H);
        StringBuilder sb = new StringBuilder("gradient readback (src->fb) :");
        int distinct = 0, prev = -1;
        for (int i = 0; i < 256; i += 1) {
            int p = bm.getPixel(rg.left + i, rg.top + 20) & 0xff;
            if (p != prev) {
                distinct++;
                prev = p;
            }
            if (i % 16 == 0) sb.append(' ').append(i).append("->").append(p);
        }
        log(sb.toString() + "  distinct levels across 256 steps=" + distinct);

        // 3) drawGraphic timing for a few sizes (reuse one buffer, 30 runs each)
        int[] sizes = {100, 400, 800, 1404};
        for (int s : sizes) {
            Object g = hw.createGb(s, s, 0x33);
            Canvas cs = hw.lockCanvas(g, null);
            paintBlack(cs, new Rect(0, 0, s, s));
            hw.unlockCanvasAndPost(g, cs);
            Rect dst = new Rect(10, 10, 10 + s, 10 + s);
            Rect src = new Rect(0, 0, s, s);
            android.graphics.RectF dstF = new android.graphics.RectF(dst);
            long t0 = System.nanoTime();
            int runs = 30;
            for (int i = 0; i < runs; i++) hw.fbCall(hw.fbDrawGraphicRect, g, src, dstF, pDef);
            log(String.format(Locale.US, "drawGraphic %dx%d : %.2f ms per call", s, s, ms(t0, System.nanoTime()) / runs));
        }
        // clean the big test area so it does not linger in the FB
        fbFill(new Rect(0, 0, W, H), Color.WHITE, pDef);
        log("--- fbtest2 done");
    }

    /** Draw with each FB drawing API into a 200x200 box and read the FB back: did the pixels land? */
    private void fbtest(Map<String, String> a) {
        if (!kind.equals("hw") || !waitReady(3000)) {
            log("fbtest needs the hw surface");
            return;
        }
        log("createDrawApi: " + hw.createDrawApi());
        int W = curSv.getWidth(), H = curSv.getHeight();
        Rect r = new Rect(500, 500, 700, 700);
        Object pDef = hw.newPaint();
        Object pEr = hw.newPaint();
        hw.paintSet(pEr, "setDitherMode", int.class, 1);
        hw.paintSet(pEr, "setFloatClip", boolean.class, true);
        hw.paintSet(pEr, "setColor", int.class, Color.BLACK);

        android.graphics.Bitmap b = hw.captureFb(W, H);
        log("capture baseline: " + (b == null ? "null" : b.getWidth() + "x" + b.getHeight()) + (b == null ? "" : " luma(r)=" + meanLuma(b, r)
                + " luma(corner)=" + meanLuma(b, new Rect(10, 10, 110, 110))));
        if (b == null) return;

        // oracle check: fill white then black with drawColor (this one is known to show on the panel)
        fbFill(r, Color.WHITE, pDef);
        log("T1 drawColor WHITE      -> luma(r)=" + captureLuma(W, H, r));
        fbFill(r, Color.BLACK, pDef);
        log("T2 drawColor BLACK      -> luma(r)=" + captureLuma(W, H, r));

        // a black graphic buffer, created exactly like the framework's eraser does: create(w, h, 1, 0x33)
        Object gb = hw.createGb(200, 200, 0x33);
        Canvas c = hw.lockCanvas(gb, null);
        paintBlack(c, new Rect(0, 0, 200, 200));
        hw.unlockCanvasAndPost(gb, c);

        fbFill(r, Color.WHITE, pDef);
        hw.fbCall(hw.fbDrawGraphic, gb, (float) r.left, (float) r.top, pDef);
        log("T3 drawGraphic(x,y)     -> luma(r)=" + captureLuma(W, H, r));

        fbFill(r, Color.WHITE, pDef);
        hw.fbCall(hw.fbDrawGraphicRect, gb, new Rect(0, 0, 200, 200), new android.graphics.RectF(r), pDef);
        log("T4 drawGraphic(rect)    -> luma(r)=" + captureLuma(W, H, r));

        fbFill(r, Color.WHITE, pDef);
        hw.fbCall(hw.fbDrawGraphicRect, gb, new Rect(0, 0, 200, 200), new android.graphics.RectF(r), pEr);
        log("T5 drawGraphic(rect)+eraser-paint -> luma(r)=" + captureLuma(W, H, r));

        fbFill(r, Color.WHITE, pDef);
        android.graphics.Path path = new android.graphics.Path();
        path.addRect(new android.graphics.RectF(r), android.graphics.Path.Direction.CW);
        hw.paintSet(pDef, "setStyle", android.graphics.Paint.Style.class, android.graphics.Paint.Style.FILL);
        hw.fbCall(hw.fbDrawPath, path, pDef);
        log("T6 drawPath(rect, FILL) -> luma(r)=" + captureLuma(W, H, r));

        fbFill(r, Color.WHITE, pDef);
        float[] pts = new float[200 * 2];
        for (int i = 0; i < 200; i++) {
            pts[i * 2] = r.left + 10 + (i % 20) * 8;
            pts[i * 2 + 1] = r.top + 10 + (i / 20) * 8;
        }
        hw.paintSet(pDef, "setStrokeWidth", float.class, 6f);
        hw.fbCall(hw.fbDrawPoints, pts, 0, pts.length, pDef);
        log("T7 drawPoints(200 dots, w=6) -> luma(r)=" + captureLuma(W, H, r));
        log("--- fbtest done");
    }

    private double captureLuma(int W, int H, Rect r) {
        android.graphics.Bitmap b = hw.captureFb(W, H);
        return b == null ? -1 : meanLuma(b, r);
    }

    private void fbFill(Rect r, int color, Object paintObj) {
        hw.fbCall(hw.fbSave);
        hw.fbCall(hw.fbClipRect, new Rect(r));
        hw.fbCall(hw.fbDrawColor, color, android.graphics.PorterDuff.Mode.SRC, paintObj);
        hw.fbCall(hw.fbRestore);
    }

    private void paintBlack(Canvas c, Rect r) {
        if (c == null) return;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.BLACK);
        c.drawRect(r, paint);
    }

    /** Keep-alive: tiny unchanged-pixel posts (2x2 white in the corner) so the T1000 never idles. */
    private void heartbeat(Map<String, String> a) {
        if (!waitReady(3000)) {
            log("surface not ready");
            return;
        }
        int dur = gi(a, "ms", 10000), gap = gi(a, "gap", 500), mode = gi(a, "m", 6);
        int size = gi(a, "size", 2);
        long end = System.nanoTime() + dur * 1000000L;
        int n = 0;
        while (System.nanoTime() < end) {
            draw(new Rect(0, 0, size, size), 255, mode, "addepdc");
            n++;
            sleep(gap);
        }
        log("heartbeat posts=" + n);
    }

    /** n small squares posted gap ms apart; each post's own blocking time is logged by draw(). */
    private void burst(Map<String, String> a) {
        if (!waitReady(3000)) {
            log("surface not ready");
            return;
        }
        int n = gi(a, "n", 20), gap = gi(a, "gap", 20), sz = gi(a, "size", 100), mode = gi(a, "m", 6);
        String via = a.containsKey("via") ? a.get("via") : "addepdc";
        int w = curSv.getWidth();
        int span = w - sz - 100;
        int pitch = gi(a, "pitch", sz + 10);
        boolean fixedRow = a.containsKey("y");
        int rowY = gi(a, "y", 300);
        long b0 = System.nanoTime();
        for (int i = 0; i < n; i++) {
            int x, y;
            if (fixedRow) {
                x = 40 + i * pitch;
                y = rowY;
            } else {
                int off = i * (sz + 10);
                x = 50 + off % span;
                y = 300 + (off / span) * (sz + 10);
            }
            Rect r = new Rect(x, y, x + sz, y + sz);
            log(String.format(Locale.US, "burst #%d start +%.1fms", i, ms(b0, System.nanoTime())));
            draw(r, 0, mode, via);
            if (gap > 0) sleep(gap);
        }
        log(String.format(Locale.US, "burst total %.1fms for %d posts", ms(b0, System.nanoTime()), n));
    }

    /** Fill r with `gray`, then optionally overlay the text given by arg s= (black, centered, size=). */
    private void paintContent(Canvas c, Rect r, int gray) {
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.rgb(gray, gray, gray));
        c.drawRect(r, paint);
        String s = curArgs.get("s");
        if (s != null) {
            paint.setColor(Color.BLACK);
            paint.setAntiAlias(true);
            paint.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            paint.setTextSize(gi(curArgs, "size", 700));
            Rect tb = new Rect();
            paint.getTextBounds(s, 0, s.length(), tb);
            c.drawText(s, r.centerX() - tb.exactCenterX(), r.centerY() - tb.exactCenterY(), paint);
            paint.setAntiAlias(false);
        }
    }

    private void plainDraw(Rect r, int gray, int mode, String via) {
        SurfaceHolder holder = curSv.getHolder();
        Surface surface = holder.getSurface();
        long t0 = System.nanoTime();
        Rect dirty = new Rect(r);
        Canvas c = holder.lockCanvas(dirty);
        if (c == null) {
            log("lockCanvas returned null");
            return;
        }
        paintContent(c, r, gray);
        long t1 = System.nanoTime();
        if (mode >= 0) {
            if (via.equals("addepdc")) {
                Eink.call(Eink.addEpdc, surface, new int[]{r.left, r.top, r.right, r.bottom, mode});
            } else if (via.equals("next")) {
                Eink.call(Eink.setForceNext, mode);
            } else if (via.equals("force")) {
                Eink.call(Eink.forceUpdateMode, surface, mode);
            }
        }
        long t2 = System.nanoTime();
        holder.unlockCanvasAndPost(c);
        long t3 = System.nanoTime();
        log(String.format(Locale.US, "plain rect=%s gray=%d mode=0x%x via=%s lockDraw=%.2fms epdc=%.2fms post=%.2fms dirty=%s",
                r.toShortString(), gray, mode, via, ms(t0, t1), ms(t1, t2), ms(t2, t3), dirty.toShortString()));
    }

    private void hwDraw(Rect r, int gray, int mode, String via) {
        long t0 = System.nanoTime();
        Object gb = hw.lockActiveBuffer();
        if (gb == null) {
            log("lockActiveBuffer returned null");
            return;
        }
        long t1 = System.nanoTime();
        Rect dirty = new Rect(r);
        Canvas c = hw.lockCanvas(gb, dirty);
        if (c == null) {
            log("GraphicBuffer.lockCanvas returned null");
            hw.unlockActiveBuffer();
            return;
        }
        paintContent(c, r, gray);
        hw.unlockCanvasAndPost(gb, c);
        long t2 = System.nanoTime();
        int m = mode < 0 ? 3 : mode;
        if (gi(curArgs, "force", 0) == 1) Eink.call(Eink.setForceNext, m);
        if (via.equals("inv")) hw.invalidate(r, m);
        else hw.postFrameBuffer(r, m);
        long t3 = System.nanoTime();
        hw.unlockActiveBuffer();
        long t4 = System.nanoTime();
        log(String.format(Locale.US, "hw rect=%s gray=%d mode=0x%x via=%s lockBuf=%.2fms draw=%.2fms post=%.2fms unlock=%.2fms",
                r.toShortString(), gray, m, via, ms(t0, t1), ms(t1, t2), ms(t2, t3), ms(t3, t4)));
        if (gi(curArgs, "ws", 0) == 1) {
            long w0 = System.nanoTime();
            hw.waitSync();
            log(String.format(Locale.US, "waitSync blocked %.2fms", ms(w0, System.nanoTime())));
        }
        int poll = gi(curArgs, "poll", 0);
        if (poll > 0) pollRefresh(t3, poll);
    }

    /** After a post: sample isMarkPenRefreshComplete()/getPowerStatus() and log every transition. */
    private void pollRefresh(long base, int durMs) {
        Boolean last = null;
        Object lastPw = null;
        StringBuilder sb = new StringBuilder();
        long end = System.nanoTime() + durMs * 1000000L;
        while (System.nanoTime() < end) {
            Object v = Eink.call(Eink.isPenRefreshComplete);
            Object pw = Eink.call(Eink.getPowerStatus);
            boolean b = (v instanceof Boolean) && ((Boolean) v);
            if (last == null || b != last || (pw != null && !pw.equals(lastPw))) {
                sb.append(String.format(Locale.US, " [%.1fms complete=%s power=%s]", ms(base, System.nanoTime()), b, pw));
                last = b;
                lastPw = pw;
            }
            java.util.concurrent.locks.LockSupport.parkNanos(500000L);
        }
        log("poll transitions:" + sb);
    }

    /** Idle watch: log power status transitions (to learn when the T1000 goes to sleep). */
    private void watchPower(int durMs) {
        Object lastPw = null, lastPmic = null;
        long base = System.nanoTime();
        long end = base + durMs * 1000000L;
        StringBuilder sb = new StringBuilder();
        while (System.nanoTime() < end) {
            Object pw = Eink.call(Eink.getPowerStatus);
            Object pmic = Eink.call(Eink.getPmicPowerStatus);
            if (!String.valueOf(pw).equals(String.valueOf(lastPw)) || !String.valueOf(pmic).equals(String.valueOf(lastPmic))) {
                sb.append(String.format(Locale.US, " [%.0fms power=%s pmic=%s]", ms(base, System.nanoTime()), pw, pmic));
                lastPw = pw;
                lastPmic = pmic;
            }
            sleep(5);
        }
        log("watch transitions:" + sb);
    }
}
