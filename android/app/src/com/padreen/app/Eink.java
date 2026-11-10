package com.padreen.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.util.Log;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;

/**
 * Huawei eink.framework access for the HMW-W09 (MatePad Paper, HarmonyOS 2.1).
 *
 * Verified on the device (docs/probe): everything used here is on the framework's *allowed* surface, no hidden-API
 * bypass. Rendering model:
 *   - HWSurfaceView owns an FB overlay (1872x1404 ARGB, starts fully transparent).
 *   - FBHandSurface.drawGraphic(GraphicBuffer, srcRect, dstRectF, paint) copies RGBA pixels into the FB 1:1.
 *   - EPDCDevice.setForceNextPostMode(mode) + FBHandSurface.postFrameBuffer(l,t,r,b,mode) pushes that region.
 * Nothing global is modified: setEpdcDefaultMode / setDelayRefreshInterval / setNeedCompareFrame ... are never called.
 */
final class Eink {
    static final String TAG = "Padreen";
    /** gralloc usage SW_READ_OFTEN | SW_WRITE_OFTEN; adding HW_TEXTURE (0x100) makes drawGraphic a silent no-op. */
    static final int GB_USAGE = 0x33;

    /** Waveform ids passed to setForceNextPostMode (values from android.eink.api.EinkMode). */
    static final int MODE_DU = 1;
    static final int MODE_GL16 = 3;
    static final int MODE_DU4_4BIT = 5;
    static final int MODE_A2 = 6;                 // "T1000_MIX_MODE" (A2) direct path
    static final int MODE_DU4_5BIT = 7;
    static final int MODE_A2_F6 = 8;              // "browsera2 mode frame 6..9": A2 with 6 / 7 / 8 / 9 drive frames (direct path)
    static final int MODE_A2_F7 = 9;
    static final int MODE_A2_F8 = 10;
    static final int MODE_A2_F9 = 11;
    static final int MODE_GC16_FULL = 98;

    private static Constructor<?> hwCtor, paintCtor;
    private static Method hwGetFb, gbCreate, gbLockCanvas, gbUnlockCanvasAndPost, gbCaptureBitmap;
    private static Method fbDrawGraphicRect, fbPost, fbSetAutoHw, fbCapture, fbSave, fbRestore, fbClipRect, fbDrawColor;
    private static Method epdcForceNext;
    /** Optional (read-only / experimental) EPDCDevice + FB entry points; null when the framework denies them. */
    private static Method epdcPower, epdcPmic, epdcLastMode, epdcForceRefresh, fbInvalidate5, viewApiInvalidate;
    private static Class<?> cGb;

    /** Experiment: invalidate an ordinary view with an explicit waveform (ViewAPI is on the allowed list). */
    static void invalidateView(View v, int waveform) {
        try {
            if (viewApiInvalidate != null) {
                viewApiInvalidate.invoke(null, v, new Rect(0, 0, v.getWidth(), v.getHeight()), waveform);
                return;
            }
        } catch (Throwable t) {
            Log.w(TAG, "ViewAPI.invalidate failed: " + t);
        }
        v.invalidate();
    }

    /** true: draw through an ordinary SurfaceView (SurfaceFlinger's normal, throttled refresh path) instead of the FB overlay. */
    static boolean useNormal;

    /** T1000 power state as reported by EPDCDevice.getPowerStatus(): 3 = on; anything else = asleep / waking. */
    static volatile int lastPower = -1;

    private static Method optional(Class<?> c, String name, Class<?>... params) {
        try {
            return c.getMethod(name, params);
        } catch (Throwable t) {
            Log.w(TAG, "optional API missing: " + c.getSimpleName() + "#" + name + " -> " + t.getClass().getSimpleName());
            return null;
        }
    }

    static int power() {
        try {
            return epdcPower == null ? -1 : (Integer) epdcPower.invoke(null);
        } catch (Throwable t) {
            return -2;
        }
    }

    static int pmic() {
        try {
            return epdcPmic == null ? -1 : (Integer) epdcPmic.invoke(null);
        } catch (Throwable t) {
            return -2;
        }
    }

    /** Debug thread: logs every change of the T1000 / PMIC power state (read-only calls). */
    static void startPowerWatch(final long periodMs) {
        if (epdcPower == null) return;
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                int lp = Integer.MIN_VALUE, lm = Integer.MIN_VALUE;
                while (true) {
                    int p = power(), m = pmic();
                    lastPower = p;
                    if (p != lp || m != lm) {
                        Log.i(TAG, "POWER t1000=" + p + " pmic=" + m + " (was " + lp + "/" + lm + ")");
                        lp = p;
                        lm = m;
                    }
                    try {
                        Thread.sleep(periodMs);
                    } catch (InterruptedException e) {
                        return;
                    }
                }
            }
        }, "pd-power");
        t.setDaemon(true);
        t.start();
    }

    /** @return null when everything needed is present, otherwise a human readable reason. */
    static String init() {
        try {
            Class<?> cHw = Class.forName("android.eink.view.HWSurfaceView");
            hwCtor = cHw.getConstructor(Context.class);
            hwGetFb = cHw.getMethod("getFBSurface");
            cGb = Class.forName("android.eink.view.GraphicBuffer");
            gbCreate = cGb.getMethod("create", int.class, int.class, int.class, int.class);
            gbLockCanvas = cGb.getMethod("lockCanvas");
            gbUnlockCanvasAndPost = cGb.getMethod("unlockCanvasAndPost", Canvas.class);
            gbCaptureBitmap = cGb.getMethod("capture", Bitmap.class);
            Class<?> cPaint = Class.forName("android.eink.view.FBPaint");
            paintCtor = cPaint.getConstructor();
            Class<?> cFb = Class.forName("android.eink.view.IFBHandSurface");
            fbDrawGraphicRect = cFb.getMethod("drawGraphic", cGb, Rect.class, RectF.class, cPaint);
            fbPost = cFb.getMethod("postFrameBuffer", int.class, int.class, int.class, int.class, int.class);
            fbSetAutoHw = cFb.getMethod("setAutoHandWrite", boolean.class);
            fbCapture = cFb.getMethod("capture", cGb);
            fbSave = cFb.getMethod("save");
            fbRestore = cFb.getMethod("restore");
            fbClipRect = cFb.getMethod("clipRect", Rect.class);
            fbDrawColor = cFb.getMethod("drawColor", int.class, android.graphics.PorterDuff.Mode.class, cPaint);
            Class<?> cEpdc = Class.forName("android.eink.EPDCDevice");
            epdcForceNext = cEpdc.getMethod("setForceNextPostMode", int.class);
            epdcPower = optional(cEpdc, "getPowerStatus");
            epdcPmic = optional(cEpdc, "getPmicPowerStatus");
            epdcLastMode = optional(cEpdc, "getEpdcLastMode");
            epdcForceRefresh = optional(cEpdc, "forceRefresh", int.class);
            fbInvalidate5 = optional(cFb, "invalidate", int.class, int.class, int.class, int.class, int.class);
            try {
                viewApiInvalidate = Class.forName("android.eink.api.ViewAPI").getMethod("invalidate", View.class, Rect.class, int.class);
            } catch (Throwable t) {
                Log.w(TAG, "optional API missing: ViewAPI#invalidate -> " + t.getClass().getSimpleName());
            }
            return null;
        } catch (Throwable t) {
            Log.e(TAG, "Eink.init failed", t);
            return t.toString();
        }
    }

    /** Set by MainActivity: app-private external dir (no permission needed). Debug dumps only. */
    static String dumpDir;

    private static void dump(Bitmap b, String name) {
        try {
            java.io.FileOutputStream o = new java.io.FileOutputStream(new java.io.File(dumpDir, name));
            b.compress(Bitmap.CompressFormat.PNG, 100, o);
            o.close();
        } catch (Throwable t) {
            Log.w(TAG, "dump " + name + " failed: " + t);
        }
    }

    /** The FB overlay and the scratch buffers used to feed it. Single-threaded: only the receiver thread draws. */
    static final class Fb {
        final View view;
        private final Object fb;
        private final Object paint;
        int width, height;

        private Object gbStage;
        private Bitmap stageBmp;
        private int[] px;
        private final int[] lut = new int[256];
        private final Rect srcRect = new Rect();
        private final RectF dstRectF = new RectF();

        private SurfaceHolder holder;           // normal path only
        private Canvas pendingCanvas;           // normal path: locked by blit*, released by post()
        private final Rect dirty = new Rect();

        Fb(Context ctx) throws Exception {
            if (useNormal) {
                view = new SurfaceView(ctx);
                fb = null;
                paint = null;
                holder = ((SurfaceView) view).getHolder();
            } else {
                view = (View) hwCtor.newInstance(ctx);
                fb = hwGetFb.invoke(view);
                paint = paintCtor.newInstance();
            }
            for (int i = 0; i < 256; i++) lut[i] = 0xFF000000 | (i << 16) | (i << 8) | i;
        }

        /**
         * Call when the surface size is known - and again whenever it changes (the device was turned: landscape 1872x1404 <-> portrait
         * 1404x1872).  Synchronized with the posts of the render thread (they hold this object's monitor).
         */
        synchronized void onSurface(int w, int h) throws Exception {
            if (stageBmp != null && w == width && h == height) return;
            width = w;
            height = h;
            // NOTE: autoHandWrite stays at the system default (true). Turning it off made the FB overlay vanish.
            if (!useNormal) {
                gbStage = gbCreate.invoke(null, w, h, 1 /* RGBA_8888 */, GB_USAGE);
                if (gbStage == null) throw new IllegalStateException("GraphicBuffer.create returned null");
            }
            abortDraw();
            Bitmap old = stageBmp;
            stageBmp = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            px = new int[w * h];
            if (old != null) old.recycle();
        }

        /** Gives back a canvas that was locked by a draw call but never posted (a failed frame must not block every later one). */
        synchronized void abortDraw() {
            Canvas c = pendingCanvas;
            pendingCanvas = null;
            if (c == null || holder == null) return;
            try {
                holder.unlockCanvasAndPost(c);
            } catch (Throwable t) {
                Log.w(TAG, "abortDraw: " + t);
            }
        }

        /** Debug: fill the whole FB with opaque white (drawColor under a clip). */
        void clearWhite() throws Exception {
            if (useNormal) return;
            fbSave.invoke(fb);
            fbClipRect.invoke(fb, new Rect(0, 0, width, height));
            fbDrawColor.invoke(fb, Color.WHITE, android.graphics.PorterDuff.Mode.SRC, paint);
            fbRestore.invoke(fb);
        }

        /** Debug: pattern R=x, G=y copied 1:1 through GraphicBuffer + drawGraphic, read back, report EXACT/WRONG. */
        String selfTest() throws Exception {
            if (useNormal) return "selftest not available on the normal path";
            StringBuilder sb = new StringBuilder();
            int[][] sizes = {{200, 200}, {1000, 700}, {1872, 1404}};
            for (int[] s : sizes) {
                int w = s[0], h = s[1];
                Object gb = gbCreate.invoke(null, w, h, 1, GB_USAGE);
                int[] p = new int[w * h];
                for (int y = 0; y < h; y++) {
                    int rowc = 0xFF000000 | ((y & 255) << 8);
                    for (int x = 0; x < w; x++) p[y * w + x] = rowc | ((x & 255) << 16);
                }
                Bitmap bm = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                bm.setPixels(p, 0, w, 0, 0, w, h);
                Canvas c = (Canvas) gbLockCanvas.invoke(gb);
                c.drawBitmap(bm, new Rect(0, 0, w, h), new Rect(0, 0, w, h), null);
                gbUnlockCanvasAndPost.invoke(gb, c);
                clearWhite();
                fbDrawGraphicRect.invoke(fb, gb, new Rect(0, 0, w, h), new RectF(0, 0, w, h), paint);
                Object gc = gbCreate.invoke(null, width, height, 1, GB_USAGE);
                fbCapture.invoke(fb, gc);
                Bitmap fbb = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                gbCaptureBitmap.invoke(gc, fbb);
                int[] probe = {16, 64, 128, 200};
                boolean exact = true;
                sb.append(w).append('x').append(h).append(" R@x=");
                for (int dx : probe) {
                    if (dx >= w) continue;
                    int r = (fbb.getPixel(dx, 5) >> 16) & 255;
                    sb.append(r).append(r == (dx & 255) ? "" : "!").append(' ');
                    if (r != (dx & 255)) exact = false;
                }
                int last = fbb.getPixel(w - 1, h - 1);
                if (((last >> 16) & 255) != ((w - 1) & 255) || ((last >> 8) & 255) != ((h - 1) & 255)) exact = false;
                sb.append(exact ? " EXACT; " : " WRONG; ");
                bm.recycle();
                fbb.recycle();
            }
            clearWhite();
            return sb.toString();
        }

        /** Debug: read the FB back and return {mean luma x100, crc32 of the luma bytes} of a region. */
        long[] verify(int x, int y, int w, int h) throws Exception {
            if (useNormal) throw new IllegalStateException("verify not available on the normal path");
            Object gb = gbCreate.invoke(null, width, height, 1, GB_USAGE);
            fbCapture.invoke(fb, gb);
            Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
            gbCaptureBitmap.invoke(gb, bmp);
            if (dumpDir != null) {
                dump(bmp, "fb.png");
                dump(stageBmp, "stage.png");
            }
            int[] row = new int[w];
            byte[] luma = new byte[w * h];
            long sum = 0;
            int o = 0;
            for (int j = 0; j < h; j++) {
                bmp.getPixels(row, 0, w, x, y + j, w, 1);
                for (int i = 0; i < w; i++) {
                    int p = row[i];
                    int g = (((p >> 16) & 0xff) * 77 + ((p >> 8) & 0xff) * 150 + (p & 0xff) * 29) >> 8;
                    luma[o++] = (byte) g;
                    sum += g;
                }
            }
            bmp.recycle();
            java.util.zip.CRC32 crc = new java.util.zip.CRC32();
            crc.update(luma, 0, luma.length);
            return new long[]{sum * 100 / (w * h), crc.getValue()};
        }

        /** Expand 8-bit gray (w*h bytes at gray[off]) into the FB at (x,y). Does not post. */
        void blitGray(byte[] gray, int off, int x, int y, int w, int h) throws Exception {
            final int n = w * h;
            final int[] p = px;
            final int[] l = lut;
            for (int i = 0; i < n; i++) p[i] = l[gray[off + i] & 0xff];
            stageBmp.setPixels(p, 0, w, 0, 0, w, h);
            if (useNormal) {
                normalDraw(stageBmp, x, y, w, h);
                return;
            }
            Canvas c = (Canvas) gbLockCanvas.invoke(gbStage);
            srcRect.set(0, 0, w, h);
            c.drawBitmap(stageBmp, srcRect, srcRect, null);
            gbUnlockCanvasAndPost.invoke(gbStage, c);
            dstRectF.set(x, y, x + w, y + h);
            fbDrawGraphicRect.invoke(fb, gbStage, srcRect, dstRectF, paint);
        }

        /** Normal path: lock only the dirty rectangle, draw the tile; post() releases (queues) the buffer. */
        private void normalDraw(Bitmap tile, int x, int y, int w, int h) {
            abortDraw();                                    // a previous frame that never got posted (it threw) still holds the lock
            dirty.set(x, y, x + w, y + h);
            Canvas c = holder.lockCanvas(dirty);
            if (c == null) throw new IllegalStateException("lockCanvas returned null");
            pendingCanvas = c;                              // from here on abortDraw() can release it if the drawing below fails
            srcRect.set(0, 0, w, h);
            Rect dst = new Rect(x, y, x + w, y + h);
            c.drawBitmap(tile, srcRect, dst, null);
        }

        /** Draw an already prepared ARGB bitmap region into the FB at (x,y). Does not post. */
        void blitBitmap(Bitmap bmp, int x, int y) throws Exception {
            int w = bmp.getWidth(), h = bmp.getHeight();
            if (useNormal) {
                normalDraw(bmp, x, y, w, h);
                return;
            }
            Canvas c = (Canvas) gbLockCanvas.invoke(gbStage);
            srcRect.set(0, 0, w, h);
            c.drawBitmap(bmp, srcRect, srcRect, null);
            gbUnlockCanvasAndPost.invoke(gbStage, c);
            dstRectF.set(x, y, x + w, y + h);
            fbDrawGraphicRect.invoke(fb, gbStage, srcRect, dstRectF, paint);
        }

        /** Choose the waveform for the next post, then post the region. */
        void post(int x, int y, int w, int h, int mode) throws Exception {
            epdcForceNext.invoke(null, mode);
            if (useNormal) {
                Canvas c = pendingCanvas;
                pendingCanvas = null;
                if (c != null) holder.unlockCanvasAndPost(c);
                return;
            }
            fbPost.invoke(fb, x, y, x + w, y + h, mode);
        }

        private int wakeToggle;

        /**
         * Experiment: how can an app wake a sleeping T1000?  method 1 = EPDCDevice.forceRefresh(mode),
         * 2 = FB invalidate(rect, mode), 3 = FB postFrameBuffer (control).  A 36x36 corner tile is toggled first so the
         * frame really differs.  Returns a one-line report incl. the time until getPowerStatus() == 3 again.
         */
        String wakeProbe(int method, int mode) throws Exception {
            final int s = 36;
            byte[] tile = new byte[s * s];
            java.util.Arrays.fill(tile, (byte) ((wakeToggle++ & 1) == 0 ? 0x00 : 0xFF));
            int x = width - s, y = height - s;
            int p0 = power();
            long t0 = System.nanoTime();
            if (method != 1) blitGray(tile, 0, x, y, s, s);
            long t1 = System.nanoTime();
            switch (method) {
                case 1:
                    if (epdcForceRefresh == null) return "forceRefresh unavailable";
                    epdcForceRefresh.invoke(null, mode);
                    break;
                case 2:
                    if (fbInvalidate5 == null) return "fb.invalidate unavailable";
                    epdcForceNext.invoke(null, mode);
                    fbInvalidate5.invoke(fb, x, y, x + s, y + s, mode);
                    break;
                default:
                    post(x, y, s, s, mode);
                    break;
            }
            long t2 = System.nanoTime();
            long deadline = t2 + 8000000000L;
            int p = power();
            while (p != 3 && System.nanoTime() < deadline) {
                Thread.sleep(5);
                p = power();
            }
            long t3 = System.nanoTime();
            return String.format(java.util.Locale.US, "method=%d mode=%d power %d -> %d  draw %.1f ms  call %.1f ms  status==3 after %.0f ms",
                    method, mode, p0, p, (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6);
        }

        /** Full-screen text message (used for the idle / waiting screen), pushed with a full refresh. */
        synchronized void showMessage(String text) {
            try {
                Bitmap bmp = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888);
                Canvas c = new Canvas(bmp);
                c.drawColor(Color.WHITE);
                Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
                p.setColor(Color.BLACK);
                p.setTextSize(54f);
                p.setTextAlign(Paint.Align.CENTER);
                String[] lines = text.split("\n");
                float y = height / 2f - (lines.length - 1) * 40f;
                for (String line : lines) {
                    c.drawText(line, width / 2f, y, p);
                    y += 80f;
                }
                blitBitmap(bmp, 0, 0);
                bmp.recycle();
                post(0, 0, width, height, MODE_GC16_FULL);
            } catch (Throwable t) {
                Log.e(TAG, "showMessage failed", t);
            }
        }
    }
}
