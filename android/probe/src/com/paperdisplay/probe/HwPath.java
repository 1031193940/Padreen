package com.paperdisplay.probe;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.util.Log;
import android.view.View;

import java.lang.reflect.Method;

/**
 * Path B: android.eink.view.HWSurfaceView + IFBHandSurface.postFrameBuffer (the pen fast path).
 * Everything via reflection. Argument order of postFrameBuffer/invalidate is (l, t, r, b, mode).
 */
final class HwPath {
    static final String TAG = "PaperProbe";

    View view;
    Method mLock, mUnlock, mGetFb, gbLockCanvasRect, gbUnlockCanvasAndPost, fbPost5, fbInv5;
    Method fbWaitSync, fbSetPenPostMode, fbSetAutoHw, fbIsAutoHw, fbToString;
    Method gbCreate, fbSave, fbRestore, fbClipRect, fbDrawColor, fbDrawGraphic, gbLockBitmap, gbUnlockBitmap;
    Method fbDrawGraphicRect, fbCapture, gbCaptureBitmap, fbDrawPath, fbDrawPoints;
    Class<?> cPaint;
    Object fbSurface;

    /** Second, independent set of reflection lookups for the FB drawing API (kept separate so a miss is obvious). */
    String createDrawApi() {
        StringBuilder sb = new StringBuilder();
        try {
            Class<?> cGb = Class.forName("android.eink.view.GraphicBuffer");
            gbCreate = cGb.getMethod("create", int.class, int.class, int.class, int.class);
            gbLockBitmap = cGb.getMethod("lockBitmap");
            gbUnlockBitmap = cGb.getMethod("unlockBitmap");
            gbCaptureBitmap = cGb.getMethod("capture", android.graphics.Bitmap.class);
            cPaint = Class.forName("android.eink.view.FBPaint");
            Class<?> cFb = Class.forName("android.eink.view.IFBHandSurface");
            fbSave = cFb.getMethod("save");
            fbRestore = cFb.getMethod("restore");
            fbClipRect = cFb.getMethod("clipRect", Rect.class);
            fbDrawColor = cFb.getMethod("drawColor", int.class, android.graphics.PorterDuff.Mode.class, cPaint);
            fbDrawGraphic = cFb.getMethod("drawGraphic", cGb, float.class, float.class, cPaint);
            fbDrawGraphicRect = cFb.getMethod("drawGraphic", cGb, Rect.class, android.graphics.RectF.class, cPaint);
            fbCapture = cFb.getMethod("capture", cGb);
            fbDrawPath = cFb.getMethod("drawPath", android.graphics.Path.class, cPaint);
            fbDrawPoints = cFb.getMethod("drawPoints", float[].class, int.class, int.class, cPaint);
            sb.append("drawApi=OK");
        } catch (Throwable t) {
            sb.append("drawApi=FAIL(").append(t).append(")");
        }
        return sb.toString();
    }

    /** Set a property on an FBPaint through its public setter, e.g. paintSet(p, "setDitherMode", int.class, 1). */
    void paintSet(Object paint, String setter, Class<?> type, Object val) {
        try {
            cPaint.getMethod(setter, type).invoke(paint, val);
        } catch (Throwable t) {
            Log.e(TAG, "FBPaint." + setter + " failed: " + t + " cause=" + t.getCause());
        }
    }

    /** Read the whole FB back into a Bitmap (FB native orientation). null on failure. */
    android.graphics.Bitmap captureFb(int w, int h) {
        try {
            Object gb = gbCreate.invoke(null, w, h, 1, 0x33);
            if (gb == null) return null;
            fbCapture.invoke(fbSurface, gb);
            android.graphics.Bitmap bmp = android.graphics.Bitmap.createBitmap(w, h, android.graphics.Bitmap.Config.ARGB_8888);
            gbCaptureBitmap.invoke(gb, bmp);
            return bmp;
        } catch (Throwable t) {
            Log.e(TAG, "captureFb failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }

    Object newPaint() {
        try {
            return cPaint.getConstructor().newInstance();
        } catch (Throwable t) {
            Log.e(TAG, "new FBPaint failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }

    Object createGb(int w, int h, int usage) {
        try {
            return gbCreate.invoke(null, w, h, 1 /* RGBA_8888 */, usage);
        } catch (Throwable t) {
            Log.e(TAG, "GraphicBuffer.create failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }

    void fbCall(Method m, Object... args) {
        try {
            m.invoke(fbSurface, args);
        } catch (Throwable t) {
            Log.e(TAG, "fb " + m.getName() + " failed: " + t + " cause=" + t.getCause());
        }
    }

    String create(Context ctx) {
        try {
            Class<?> cHw = Class.forName("android.eink.view.HWSurfaceView");
            view = (View) cHw.getConstructor(Context.class).newInstance(ctx);
            mLock = cHw.getMethod("lockActiveBuffer");
            mUnlock = cHw.getMethod("unlockActiveBuffer");
            mGetFb = cHw.getMethod("getFBSurface");
            Class<?> cGb = Class.forName("android.eink.view.GraphicBuffer");
            gbLockCanvasRect = cGb.getMethod("lockCanvas", Rect.class);
            gbUnlockCanvasAndPost = cGb.getMethod("unlockCanvasAndPost", Canvas.class);
            Class<?> cFb = Class.forName("android.eink.view.IFBHandSurface");
            fbPost5 = cFb.getMethod("postFrameBuffer", int.class, int.class, int.class, int.class, int.class);
            fbInv5 = cFb.getMethod("invalidate", int.class, int.class, int.class, int.class, int.class);
            fbWaitSync = cFb.getMethod("waitSync");
            fbSetPenPostMode = cFb.getMethod("setAhwPenPostMode", int.class);
            fbSetAutoHw = cFb.getMethod("setAutoHandWrite", boolean.class);
            fbIsAutoHw = cFb.getMethod("isAutoHandWrite");
            fbSurface = mGetFb.invoke(view);
            return "HWSurfaceView=OK fbSurface=" + fbSurface + " autoHandWrite=" + fbIsAutoHw.invoke(fbSurface);
        } catch (Throwable t) {
            Log.e(TAG, "HwPath.create failed", t);
            return "HWSurfaceView=FAIL(" + t + " cause=" + t.getCause() + ")";
        }
    }

    void waitSync() {
        try {
            fbWaitSync.invoke(fbSurface);
        } catch (Throwable t) {
            Log.e(TAG, "waitSync failed: " + t + " cause=" + t.getCause());
        }
    }

    void setPenPostMode(int m) {
        try {
            fbSetPenPostMode.invoke(fbSurface, m);
        } catch (Throwable t) {
            Log.e(TAG, "setAhwPenPostMode failed: " + t + " cause=" + t.getCause());
        }
    }

    void setAutoHandWrite(boolean b) {
        try {
            fbSetAutoHw.invoke(fbSurface, b);
        } catch (Throwable t) {
            Log.e(TAG, "setAutoHandWrite failed: " + t + " cause=" + t.getCause());
        }
    }

    Object lockActiveBuffer() {
        try {
            return mLock.invoke(view);
        } catch (Throwable t) {
            Log.e(TAG, "lockActiveBuffer failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }

    Canvas lockCanvas(Object gb, Rect r) {
        try {
            return (Canvas) gbLockCanvasRect.invoke(gb, r);
        } catch (Throwable t) {
            Log.e(TAG, "GraphicBuffer.lockCanvas failed: " + t + " cause=" + t.getCause());
            return null;
        }
    }

    void unlockCanvasAndPost(Object gb, Canvas c) {
        try {
            gbUnlockCanvasAndPost.invoke(gb, c);
        } catch (Throwable t) {
            Log.e(TAG, "GraphicBuffer.unlockCanvasAndPost failed: " + t + " cause=" + t.getCause());
        }
    }

    void postFrameBuffer(Rect r, int mode) {
        try {
            fbPost5.invoke(fbSurface, r.left, r.top, r.right, r.bottom, mode);
        } catch (Throwable t) {
            Log.e(TAG, "postFrameBuffer failed: " + t + " cause=" + t.getCause());
        }
    }

    void invalidate(Rect r, int mode) {
        try {
            fbInv5.invoke(fbSurface, r.left, r.top, r.right, r.bottom, mode);
        } catch (Throwable t) {
            Log.e(TAG, "fb.invalidate failed: " + t + " cause=" + t.getCause());
        }
    }

    void unlockActiveBuffer() {
        try {
            mUnlock.invoke(view);
        } catch (Throwable t) {
            Log.e(TAG, "unlockActiveBuffer failed: " + t + " cause=" + t.getCause());
        }
    }
}
