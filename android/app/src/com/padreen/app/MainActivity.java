package com.padreen.app;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Color;
import android.graphics.PixelFormat;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import android.view.Display;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

import java.util.Locale;

/**
 * Full-screen host for the e-ink surface.  It follows the device orientation (landscape or portrait; the PC's virtual monitor for this
 * tablet is turned with it, see {@link OrientSync}) and shows what the PC sends: the session itself (network, decoder, shadow copy of the
 * panel) lives in {@link DisplayHub}, so leaving this activity - Back, Home, another app, screen off - does NOT end the session.  While
 * the surface exists the hub draws on it; when the user comes back the whole panel is repainted.  While Padreen is in front and a PC is
 * attached the hardware volume keys change the PC's volume, never the tablet's (see {@link Receiver#handleVolumeKey}).  No system
 * settings are changed.
 *
 * Who turns the activity: the system's own orientation listener has no sensor behind it on this tablet, so while "auto rotate" is on and the
 * PC does not lock the orientation, this activity reads the accelerometer itself ({@link OrientationDetector}) and asks for an explicit
 * orientation (portrait / landscape / reverse portrait / reverse landscape = display rotation 0..3).  A tablet lying flat cannot be turned by
 * sensor and simply keeps the orientation it shows.  That orientation also survives this activity: the hub remembers the last display rotation
 * ({@link DisplayHub#lastRotation}) and a new activity (Back, the home screen) starts in it instead of the manifest's landscape.
 */
public class MainActivity extends Activity implements DisplayHub.Listener {
    static final String TAG = "Padreen";
    /** A display change this soon after our own orientation request is the system carrying it out, not somebody else turning the display. */
    private static final long OWN_REQUEST_GRACE_MS = 2500;
    /** While the sensor is followed, the display's real rotation is compared with the detector's belief this often. */
    private static final long RECONCILE_MS = 2000;

    private FrameLayout root;
    private Eink.Fb fb;
    private DisplayHub hub;
    private boolean fbSized;
    private boolean lanLaunch;                  // started from the home screen (not by Padreen over adb)
    private View wakeView;
    private TouchGate gate;                  // three-finger gestures + hold-back of the first fingers (see TouchGate)
    private int lastRotation = -1;              // Display.getRotation() at the last look
    private final Handler ui = new Handler(Looper.getMainLooper());
    private DisplayManager displayManager;

    // ---- orientation: who turns the activity ------------------------------------------------------------------------------------
    private SensorManager sensorManager;
    private Sensor accelerometer;               // null: this device has none
    private OrientationDetector detector;
    private boolean started;                    // between onStart and onStop
    private boolean resumed;                    // between onResume and onPause: this activity is the one in front
    private boolean autoActive;                 // we follow the device with the accelerometer (policy says so and the activity is started)
    private boolean sensorOn;                   // the listener is registered
    private boolean sensorBroken;               // registering failed: the system's own FULL_SENSOR is all we have
    private boolean noSensorLogged;
    private long lastOwnRequestMs;              // elapsedRealtime of our last setRequestedOrientation
    private long nextReconcileMs;               // elapsedRealtime of the next look at the display's real rotation
    private long sensorLogUntilMs, sensorLogNextMs;     // debug: `am start ... --ei sensorlog 20` (elapsedRealtime; 0 = off)
    private final Runnable endSensorLogTask = new Runnable() {
        @Override
        public void run() {
            sensorLogUntilMs = 0;
            Log.i(TAG, "sensorlog finished");
            syncSensor();
        }
    };

    private final DisplayManager.DisplayListener displayListener = new DisplayManager.DisplayListener() {
        @Override
        public void onDisplayAdded(int id) {
        }

        @Override
        public void onDisplayRemoved(int id) {
        }

        @Override
        public void onDisplayChanged(int id) {
            if (id != Display.DEFAULT_DISPLAY) return;
            int rot = getWindowManager().getDefaultDisplay().getRotation();
            int prev = lastRotation;
            lastRotation = rot;
            if (prev >= 0 && rot != prev) Log.i(TAG, "display rotation " + prev + " -> " + rot + " (" + OrientationDetector.name(rot) + ")");
            rememberRotation(rot);
            // landscape <-> reverse landscape (or portrait <-> reverse portrait): the surface keeps its size, but the panel shows the picture
            // turned by 180 degrees and the system only redraws what changed - so repaint all of it.  (A change between landscape and portrait
            // gets a new surface size: surfaceChanged handles that.)
            if (prev >= 0 && rot != prev && (rot % 2) == (prev % 2)) {
                Log.i(TAG, "display turned by 180 degrees (rotation " + prev + " -> " + rot + "): full repaint");
                ui.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (hub != null && hub.renderer.hasContent()) hub.renderer.requestFull();
                    }
                }, 500);
            }
            followDisplay(rot);
        }
    };
    private final DisplayHub.OrientListener orientListener = new DisplayHub.OrientListener() {
        @Override
        public void onForceOrientation(boolean landscape) {
            applyPolicyOnUiThread();
        }

        @Override
        public void onReleaseOrientation() {
            applyPolicyOnUiThread();
        }

        private void applyPolicyOnUiThread() {
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    applyOrientationPolicy();
                }
            });
        }
    };

    // ---------------------------------------------------------------------------------------------------------------- orientation
    /** The explicit orientation request that gives this display rotation (0 = natural = portrait, 1 = landscape, 2 = reverse portrait, 3 = reverse landscape). */
    private static int requestOf(int rotation) {
        switch (OrientationDetector.normalize(rotation)) {
            case 1:
                return ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
            case 2:
                return ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT;
            case 3:
                return ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE;
            default:
                return ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
        }
    }

    /** The display rotation an explicit orientation request leads to; -1 for anything else (unspecified, FULL_SENSOR ...). */
    private static int rotationOf(int request) {
        switch (request) {
            case ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
                return 0;
            case ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
                return 1;
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
                return 2;
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
                return 3;
            default:
                return -1;
        }
    }

    private int displayRotation() {
        return getWindowManager().getDefaultDisplay().getRotation();
    }

    /**
     * The display was seen in this rotation while this activity is the one in front: the hub remembers it, so that the next display activity
     * (Back, the home screen destroy this one, the PC's monitor keeps its shape) starts in it.  Not while another activity is in front: the
     * landscape-only home screen turns the display too, and that is not where the desktop is.  Not while the system is still carrying out
     * a request of ours either: what it shows then is the rotation before.
     */
    private void rememberRotation(int rot) {
        if (!resumed || hub == null) return;
        int requested = rotationOf(getRequestedOrientation());
        if (requested >= 0 && rot != requested && SystemClock.elapsedRealtime() - lastOwnRequestMs <= OWN_REQUEST_GRACE_MS) return;
        hub.setLastRotation(rot);
    }

    /** We turn (or keep) the display to this rotation ourselves: remembered at once, the system follows a moment later. */
    private void rememberIntended(int rot) {
        if (hub != null) hub.setLastRotation(rot);
    }

    /**
     * Where this activity is (or is just going): the rotation of its own explicit orientation request if it has one - a new activity asks for the
     * hub's remembered rotation in onCreate, and the system carries a request out a moment after onStart / onResume, so Display.getRotation() can
     * still show the previous activity's rotation (the landscape-only home screen) - otherwise what the display shows now.
     */
    private int knownRotation() {
        int r = rotationOf(getRequestedOrientation());
        return r >= 0 ? r : displayRotation();
    }

    /**
     * Who decides the orientation now:
     *  - the PC (it does not follow the tablet's turning): the shape it delivers, in the direction the display already has if that fits;
     *  - the user's "auto rotate" is off: landscape, same direction rule;
     *  - otherwise the tablet follows the device: our accelerometer detector turns it (even when the system's rotation lock is on: that lock is
     *    meant for the tablet's own apps); a tablet lying flat decides nothing and keeps what it shows.  Without an accelerometer the system's own
     *    FULL_SENSOR is requested, as before.
     * Whenever the detector starts following (onStart, the PC lets go, auto rotate switched on) it begins at where the display is - no visible change.
     */
    private void applyOrientationPolicy() {
        int lock = hub != null ? hub.lockedOrientation() : 0;
        boolean autoRotate = lock == 0 && new PcStore(this).autoRotate();
        setAutoActive(autoRotate && accelerometer != null && !sensorBroken && started);
        boolean sensorUsable = accelerometer != null && !sensorBroken;       // (registering may just have failed)
        int want;
        if (lock != 0) {
            want = requestOf(OrientationDetector.rotationForShape(lock == 1, knownRotation()));
        } else if (!autoRotate) {
            want = requestOf(OrientationDetector.rotationForShape(true, knownRotation()));
        } else if (autoActive) {
            want = requestOf(detector.current());
        } else if (sensorUsable) {
            want = getRequestedOrientation();                 // not started yet (onCreate): the orientation onCreate asked for stays until onStart
        } else {
            if (!noSensorLogged) {
                noSensorLogged = true;
                Log.w(TAG, "no usable accelerometer: the system's own sensor (FULL_SENSOR) decides the orientation");
            }
            want = ActivityInfo.SCREEN_ORIENTATION_FULL_SENSOR;
        }
        if (getRequestedOrientation() != want) {
            Log.i(TAG, "requested orientation " + getRequestedOrientation() + " -> " + want + " (locked by the PC: " + lock + ", "
                    + (lock != 0 ? "PC" : !autoRotate ? "fixed" : autoActive ? "accelerometer" : "system sensor") + ")");
            lastOwnRequestMs = SystemClock.elapsedRealtime();
            setRequestedOrientation(want);
            if (rotationOf(want) >= 0) rememberIntended(rotationOf(want));
        }
    }

    /**
     * The display is in rotation `rot`.  If the detector believes another one and it is not our own request that is still on its way, somebody
     * else turned the display (the system) or did not carry our request out: the detector must know, or it would fight them - it follows, and the
     * rotation is pinned with an explicit request.  Called for every display change and every couple of seconds while the sensor is followed.
     */
    private void followDisplay(int rot) {
        if (!resumed || !autoActive || detector == null || rot == detector.current()) return;     // (another activity in front turns the display too: not our business)
        if (SystemClock.elapsedRealtime() - lastOwnRequestMs <= OWN_REQUEST_GRACE_MS) return;
        Log.i(TAG, "display rotation is " + rot + " (" + OrientationDetector.name(rot) + ") but the detector thought " + OrientationDetector.name(detector.current())
                + ": not turned by us - following it");
        detector.setCurrent(rot);
        rememberRotation(rot);
        applyOrientationPolicy();
    }

    /** Starts / stops following the device with the accelerometer. */
    private void setAutoActive(boolean on) {
        if (on == autoActive) return;
        if (on) {
            detector = new OrientationDetector(knownRotation());      // a fresh start: nothing of an earlier filter state is left over
            autoActive = true;
            nextReconcileMs = SystemClock.elapsedRealtime() + RECONCILE_MS;
            lastOwnRequestMs = SystemClock.elapsedRealtime();   // the system may still be carrying out an earlier request of ours (coming back to the front)
            if (!registerSensor()) {                          // cannot read the sensor: the system's own has to do
                autoActive = false;
                sensorBroken = true;
                Log.w(TAG, "cannot register the accelerometer: falling back to the system's own orientation sensor (FULL_SENSOR)");
                return;
            }
            Log.i(TAG, "following the device with the accelerometer " + accelerometer.getName() + " (display rotation " + displayRotation() + ", requested "
                    + getRequestedOrientation() + ", detector starts at " + OrientationDetector.name(detector.current()) + ")");
        } else {
            autoActive = false;
            syncSensor();
        }
    }

    private boolean sensorLogActive() {
        return sensorLogUntilMs != 0 && SystemClock.elapsedRealtime() < sensorLogUntilMs;
    }

    /** The listener is registered exactly while the activity is started and somebody needs the samples (auto mode or the debug log). */
    private void syncSensor() {
        if (started && accelerometer != null && (autoActive || sensorLogActive())) registerSensor();
        else unregisterSensor();
    }

    private boolean registerSensor() {
        if (sensorOn) return true;
        if (sensorManager == null || accelerometer == null) return false;
        try {
            sensorOn = sensorManager.registerListener(sensorListener, accelerometer, SensorManager.SENSOR_DELAY_UI);
        } catch (RuntimeException e) {
            Log.w(TAG, "registerListener failed: " + e);
            sensorOn = false;
        }
        return sensorOn;
    }

    private void unregisterSensor() {
        if (!sensorOn) return;
        sensorOn = false;
        try {
            sensorManager.unregisterListener(sensorListener);
        } catch (RuntimeException e) {
            Log.w(TAG, "unregisterListener failed: " + e);
        }
    }

    private final SensorEventListener sensorListener = new SensorEventListener() {
        @Override
        public void onSensorChanged(SensorEvent e) {
            if (!resumed || detector == null || e.values == null || e.values.length < 3) return;      // only the activity in front turns the display
            float x = e.values[0], y = e.values[1], z = e.values[2];
            int turn = detector.onSample(x, y, z, e.timestamp / 1000000L);
            long now = SystemClock.elapsedRealtime();
            if (sensorLogUntilMs != 0 && now < sensorLogUntilMs && now >= sensorLogNextMs) {
                sensorLogNextMs = now + 500;                  // twice a second
                Log.i(TAG, String.format(Locale.US, "sensorlog x=%.2f y=%.2f z=%.2f  phi=%.1f inPlane=%.2f tilt=%.1f |a|=%.2f  detector=%s display=%d%s", x, y, z,
                        detector.phiDeg(), detector.inPlane(), detector.tiltDeg(), detector.magnitude(), OrientationDetector.name(detector.current()), displayRotation(),
                        autoActive ? "" : "  (not following the sensor now)"));
            }
            if (turn >= 0 && autoActive) sensorTurned(turn);
            if (autoActive && now >= nextReconcileMs) {       // now and then: does the display really show what we believe (a request the system did not carry out)?
                nextReconcileMs = now + RECONCILE_MS;
                followDisplay(displayRotation());
            }
        }

        @Override
        public void onAccuracyChanged(Sensor sensor, int accuracy) {
        }
    };

    /** The detector decided: the tablet is held in another orientation now. */
    private void sensorTurned(int rotation) {
        if (hub != null && hub.lockedOrientation() != 0) {    // the PC locked the orientation a moment ago: that wins
            applyOrientationPolicy();
            return;
        }
        Log.i(TAG, String.format(Locale.US, "sensor orientation -> %s (phi=%.0f inPlane=%.1f tilt=%.0f)", OrientationDetector.name(rotation), detector.phiDeg(),
                detector.inPlane(), detector.tiltDeg()));
        requestRotation(rotation);
        rememberIntended(rotation);                           // the next display activity starts the way the display is turned now
    }

    private void requestRotation(int rotation) {
        int want = requestOf(rotation);
        if (getRequestedOrientation() != want) {
            lastOwnRequestMs = SystemClock.elapsedRealtime();
            setRequestedOrientation(want);
        }
    }

    /** Debug: `am start -n com.padreen.app/.MainActivity --ei sensorlog 20` logs the raw and the filtered accelerometer twice a second for 20 s (0 = off). */
    private void startSensorLog(int seconds) {
        ui.removeCallbacks(endSensorLogTask);
        if (seconds <= 0) {
            sensorLogUntilMs = 0;
            Log.i(TAG, "sensorlog off");
            syncSensor();
            return;
        }
        if (accelerometer == null) {
            Log.w(TAG, "sensorlog: this device has no accelerometer");
            return;
        }
        if (detector == null) detector = new OrientationDetector(knownRotation());
        sensorLogUntilMs = SystemClock.elapsedRealtime() + seconds * 1000L;
        sensorLogNextMs = 0;
        Log.i(TAG, "sensorlog on for " + seconds + " s: x y z = raw m/s^2 (natural portrait axes: x right, y up the long side, z out of the screen); phi = direction of up from the"
                + " filtered vector (0 = natural portrait upright, +90 = landscape, -90 = reverse landscape, 180 = reverse portrait); inPlane = gravity in the screen plane"
                + " (flat below " + OrientationDetector.FLAT_IN_PLANE + "); tilt = degrees from flat; display = Display.getRotation()");
        ui.postDelayed(endSensorLogTask, seconds * 1000L + 300);
        syncSensor();
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        root = new FrameLayout(this);
        root.setBackgroundColor(Color.WHITE);
        setContentView(root);
        hideSystemUi();
        lanLaunch = getIntent().getBooleanExtra("lan", false);

        String err = Eink.init();
        java.io.File ext = getExternalFilesDir(null);
        if (ext != null) Eink.dumpDir = ext.getAbsolutePath();
        // debug: `am start ... --ez trace true` logs every post; `--ei pw 50` logs every T1000 power-state change (polled every 50 ms)
        Renderer.trace = getIntent().getBooleanExtra("trace", false);
        int pw = getIntent().getIntExtra("pw", 0);
        if (err == null && pw > 0) Eink.startPowerWatch(pw);
        if (err != null) {
            fail("This device has no usable Huawei e-ink framework:\n" + err);
            return;
        }
        try {
            Eink.useNormal = !"fb".equals(getIntent().getStringExtra("path"));   // normal (throttled, wake-aware) path unless `--es path fb`
            Log.i(TAG, "backend=" + (Eink.useNormal ? "normal SurfaceView" : "FB overlay"));
            fb = new Eink.Fb(this);
        } catch (Throwable t) {
            Log.e(TAG, "cannot create HWSurfaceView", t);
            fail("Cannot create the e-ink surface:\n" + t);
            return;
        }
        hub = DisplayHub.get(this);
        // A new display activity (Back / back from the home screen destroyed the old one) starts in the orientation the old one had.  The
        // manifest says landscape, which would turn a portrait desktop: the surface would get the other shape and the PC would re-shape its
        // monitor.  Asked for here, before the first policy call and any layout, so the window never shows the wrong direction.
        int startRotation = OrientationDetector.startRotation(hub.lockedOrientation(), new PcStore(this).autoRotate(), hub.lastRotation());
        Log.i(TAG, "starting in " + OrientationDetector.name(startRotation) + " (remembered: " + OrientationDetector.name(hub.lastRotation()) + ", PC lock " + hub.lockedOrientation() + ")");
        if (getRequestedOrientation() != requestOf(startRotation)) {
            lastOwnRequestMs = SystemClock.elapsedRealtime();
            setRequestedOrientation(requestOf(startRotation));
        }
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        accelerometer = sensorManager != null ? sensorManager.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) : null;
        applyOrientationPolicy();              // the accelerometer takes over in onStart; without one the system's sensor is requested (no extra rotation animation)
        if (getIntent().hasExtra("sensorlog")) startSensorLog(getIntent().getIntExtra("sensorlog", 0));
        gate = new TouchGate(new TouchGate.Sink() {
            @Override
            public void touch(android.view.MotionEvent e) {
                hub.receiver.sendTouch(e);
            }

            @Override
            public void cancelAll() {
                hub.receiver.cancelTouches();
            }

            @Override
            public void refresh() {
                hub.receiver.requestFullRefresh("three-finger tap");
            }

            @Override
            public void swipe(int kind) {
                hub.receiver.sendGesture(kind);
            }
        });
        gate.setEnabled(new PcStore(this).gestures());
        root.addView(fb.view, new FrameLayout.LayoutParams(-1, -1));
        // a tiny ordinary view above the surface (kept from the wake experiments: it makes SurfaceFlinger compose a normal frame)
        wakeView = new View(this);
        wakeView.setBackgroundColor(Color.WHITE);
        FrameLayout.LayoutParams wlp = new FrameLayout.LayoutParams(8, 8);
        wlp.leftMargin = 0;
        wlp.topMargin = 0;
        root.addView(wakeView, wlp);
        // SurfaceView defaults to RGB_565 and HWSurfaceView sizes its FB overlay from the surface format: left at the default the FB is
        // 16 bpp and every 32-bit read/write of it is garbled.  (`am start ... --es fmt 565` re-creates the old default for A/B testing.)
        if (!"565".equals(getIntent().getStringExtra("fmt"))) {
            ((SurfaceView) fb.view).getHolder().setFormat(PixelFormat.RGBA_8888);
        }
        ((SurfaceView) fb.view).getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int w, int h) {
                int rot = getWindowManager().getDefaultDisplay().getRotation();
                Log.i(TAG, "surfaceChanged " + w + "x" + h + " displayRotation=" + rot);
                lastRotation = rot;
                surfaceW = w;
                surfaceH = h;
                // A new display activity gets its first surface while the display still has the rotation of whatever was in front before (the
                // landscape-only home screen); the rotation we asked for in onCreate follows a moment later.  A surface of the wrong shape must not
                // reach the PC (it would re-shape its monitor to landscape and back): wait for the right one - or give up after the grace time.
                int want = rotationOf(getRequestedOrientation());
                long sinceRequest = SystemClock.elapsedRealtime() - lastOwnRequestMs;
                if (want >= 0 && !OrientationDetector.fits(want, w, h) && sinceRequest <= OWN_REQUEST_GRACE_MS) {
                    Log.i(TAG, "surface " + w + "x" + h + " still has the shape of the previous rotation (asked for " + OrientationDetector.name(want) + "): waiting for the turn");
                    ui.removeCallbacks(surfaceFallback);
                    ui.postDelayed(surfaceFallback, Math.max(100, OWN_REQUEST_GRACE_MS - sinceRequest));
                    return;
                }
                ui.removeCallbacks(surfaceFallback);
                applySurface(w, h, rot);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                Log.i(TAG, "surfaceDestroyed");
                ui.removeCallbacks(surfaceFallback);
                gate.reset();
                hub.detach(fb);                    // the session keeps running without a surface
            }
        });
    }

    private int surfaceW, surfaceH;             // the latest surface callback (a surface that waits for the display to turn)
    private final Runnable surfaceFallback = new Runnable() {
        @Override
        public void run() {
            Log.i(TAG, "the display did not turn to what we asked for: using the surface as it is (" + surfaceW + "x" + surfaceH + ")");
            applySurface(surfaceW, surfaceH, getWindowManager().getDefaultDisplay().getRotation());
        }
    };

    /** The surface has its final shape: size the buffers and the picture to it, ask the PC for a desktop of this shape if that changed, and draw. */
    private void applySurface(int w, int h, int rot) {
        if (OrientationDetector.fits(rot, w, h)) rememberRotation(rot);       // (a rotation one step behind the surface is not worth remembering)
        try {
            boolean resized = fbSized && (w != fb.width || h != fb.height);
            if (resized) {                  // the device was turned: whatever finger / gesture was in progress belongs to the old orientation
                gate.reset();
                hub.receiver.cancelTouches();
            }
            fb.onSurface(w, h);             // the surface's own buffers first (a no-op for the same size) ...
            hub.surfaceSize(w, h);          // ... then the picture: wipes the shadow copy and asks the PC for a desktop of this shape (no-op if unchanged)
            fbSized = true;
        } catch (Throwable t) {
            Log.e(TAG, "onSurface failed", t);
            fail("Cannot initialise the frame buffer:\n" + t);
            return;
        }
        hub.attach(fb);                    // draws from now on; repaints the whole panel from the shadow copy when a session is running
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        if (intent.getBooleanExtra("lan", false)) lanLaunch = true;
        if (intent.hasExtra("trace")) {                           // m start -n .../.MainActivity --ez trace true: log every post (diagnostics), no restart needed
            Renderer.trace = intent.getBooleanExtra("trace", false);
            Log.i(TAG, "trace " + (Renderer.trace ? "on" : "off"));
        }
        if (intent.hasExtra("sensorlog")) startSensorLog(intent.getIntExtra("sensorlog", 0));
        if (intent.hasExtra("orient") && hub != null) {           // debug (no need to turn the device): --ei orient 0 = landscape, 1 = portrait, 2 = back to the normal policy
            int o = intent.getIntExtra("orient", 2);
            Log.i(TAG, "debug: orient " + o);
            if (o == 2) {
                applyOrientationPolicy();
            } else {
                int r = o == 0 ? 1 : 0;                           // (a tilted tablet is turned back by the sensor after a moment)
                if (detector != null) detector.setCurrent(r);
                requestRotation(r);
            }
        }
    }

    @Override
    protected void onStart() {
        super.onStart();
        started = true;
        if (hub != null) {
            hub.addListener(this);
            hub.setOrientListener(orientListener);
            applyOrientationPolicy();                         // the setting may have changed on the home screen, or the PC may not follow meanwhile; starts the accelerometer
            syncSensor();                                     // (and the debug log's, if it is still running)
            check();
        }
        PcStore store = new PcStore(this);
        if (gate != null) gate.setEnabled(store.gestures());
        lastRotation = getWindowManager().getDefaultDisplay().getRotation();
        displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
        if (displayManager != null) displayManager.registerDisplayListener(displayListener, ui);
    }

    @Override
    protected void onStop() {
        started = false;
        setAutoActive(false);                                 // unregisters the accelerometer (the debug log's too)
        syncSensor();
        if (displayManager != null) displayManager.unregisterDisplayListener(displayListener);
        if (hub != null) {
            hub.removeListener(this);
            hub.setOrientListener(null);
        }
        super.onStop();
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
    }

    @Override
    protected void onPause() {
        resumed = false;                                      // from here on display changes belong to whatever comes to the front (the home screen is landscape-only)
        super.onPause();
    }

    @Override
    public void onSessionChanged() {
        runOnUiThread(new Runnable() {
            @Override
            public void run() {
                applyOrientationPolicy();                     // a new session may find a PC that follows the sensor again
                check();
            }
        });
    }

    /** A display opened from the home screen has nothing to show once its session is over for good (not while it reconnects). */
    private void check() {
        if (lanLaunch && hub != null && !hub.active() && !hub.reconnecting()) finish();
    }

    private void fail(String msg) {
        TextView tv = new TextView(this);
        tv.setText(msg);
        tv.setTextColor(Color.BLACK);
        tv.setTextSize(24f);
        tv.setPadding(48, 48, 48, 48);
        root.addView(tv);
    }

    /**
     * Every touch of the activity goes through the gate: finger / stylus events reach the PC (finger / stylus input feedback), except the
     * tablet's own three-finger gestures (tap = full refresh, swipe up / down = desktops / window switcher), which are consumed here and
     * never reach the PC.
     */
    @Override
    public boolean dispatchTouchEvent(android.view.MotionEvent ev) {
        if (gate != null) gate.onTouch(ev);
        return true;
    }

    /**
     * While Padreen is in front and a PC is attached (a session, or its Wi-Fi link being re-established) the hardware volume keys belong to
     * the PC: swallowed here, so the tablet's volume and its volume panel never react, and sent on to the PC.  The user's switch on the home
     * screen is read for every key event.  With no PC (or the switch off) the system handles the keys as usual.  Same rule on the home screen.
     */
    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        if (hub != null && hub.receiver.handleVolumeKey(ev)) return true;
        return super.dispatchKeyEvent(ev);
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemUi();
    }

    @Override
    protected void onDestroy() {
        started = false;
        ui.removeCallbacks(endSensorLogTask);
        unregisterSensor();
        if (hub != null && fb != null) hub.detach(fb);
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
}
