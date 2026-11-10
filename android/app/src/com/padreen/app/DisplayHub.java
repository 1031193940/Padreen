package com.padreen.app;

import android.content.Context;
import android.os.SystemClock;
import android.util.Log;
import android.view.View;

import java.io.IOException;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Process-wide owner of the display session: the network session (USB loopback server / LAN client), the decoder state and the renderer with
 * its shadow copy of the panel.  The activities only come and go: {@link MainActivity} attaches its e-ink surface while it is visible and
 * detaches it when the user switches to another app - the session keeps running underneath (kept alive by {@link SessionService}), and when
 * the display comes back the whole panel is repainted from the shadow buffer.  A LAN session that breaks by itself is re-established in
 * the background.  Only an explicit "disconnect", the PC saying BYE, or giving up the reconnect ends it.
 */
final class DisplayHub {
    static final String TAG = "Padreen";
    /** The panel scans 1872x1404 (landscape); the display activity can also be portrait (1404x1872) - see {@link #surfaceSize}. */
    static final int DEF_W = 1872, DEF_H = 1404;

    private static DisplayHub inst;

    static synchronized DisplayHub get(Context c) {
        if (inst == null) inst = new DisplayHub(c.getApplicationContext());
        return inst;
    }

    /** The hub if the process already has one (never creates it). */
    static synchronized DisplayHub peek() {
        return inst;
    }

    interface Listener {
        void onSessionChanged();
    }

    /** Implemented by the display activity: the PC does not follow the sensor, so turn to its orientation and stay there - or follow the sensor again (see {@link OrientSync}). */
    interface OrientListener {
        void onForceOrientation(boolean landscape);

        void onReleaseOrientation();
    }

    final Context app;
    final Renderer renderer;
    final Receiver receiver;
    private volatile Eink.Fb fb;
    private final CopyOnWriteArrayList<Listener> listeners = new CopyOnWriteArrayList<Listener>();
    private volatile OrientListener orientListener;
    /** 0 = the activity follows the sensor, 1 / 2 = locked to landscape / portrait because the PC does not follow (cleared when a session starts or the PC follows again). */
    private volatile int lockedOrientation;
    /**
     * The display rotation (0..3 as Display.getRotation(): 0 = natural = portrait, 1 = landscape) the display activity was last seen in.  The
     * activity comes and goes (Back, the home screen) but the PC's monitor keeps its shape, so a new activity starts in this rotation instead
     * of the manifest's landscape.  Process lifetime; 1 = landscape until an activity has told otherwise.
     */
    private volatile int lastRotation = 1;

    private volatile boolean active;
    private volatile boolean reconnecting;
    private volatile boolean userDisconnect;
    private volatile String kind = "";        // "USB" | "LAN"
    private volatile String label = "";
    private volatile PcStore.Pc lanPc;        // the PC of the LAN session (for reconnecting)

    private DisplayHub(Context app) {
        this.app = app;
        renderer = new Renderer(DEF_W, DEF_H, 60);
        Thread rt = new Thread(renderer, "pd-render");
        rt.setPriority(Thread.MAX_PRIORITY);
        rt.start();
        receiver = new Receiver(app, this, renderer);
        Thread t = new Thread(receiver, "pd-receiver");
        t.setPriority(Thread.MAX_PRIORITY);
        t.start();
    }

    // ------------------------------------------------------------------------------------------------------------ surface
    void attach(Eink.Fb f) {
        fb = f;
        renderer.attach(f);
        if (!active && !reconnecting && !renderer.hasContent()) showMessage(idleMessage());
    }

    void detach(Eink.Fb f) {
        if (fb == f) fb = null;
        renderer.detach(f);
    }

    /**
     * The display surface has this logical size (called by the display activity for every surfaceChanged, before it re-allocates its own
     * buffers).  A new shape (the device was turned) wipes the shadow copy of the panel and makes the PC deliver a desktop of that shape.
     */
    void surfaceSize(int w, int h) {
        renderer.resize(w, h);
        receiver.onSurfaceSize(w, h);
    }

    void setOrientListener(OrientListener l) {
        orientListener = l;
    }

    /** 0 = follow the sensor, 1 = landscape, 2 = portrait (locked because the PC does not follow). */
    int lockedOrientation() {
        return lockedOrientation;
    }

    /** Any thread: the PC does not deliver the shape of the surface (cannot, may not, or never answered): turn the activity to the PC's orientation and keep it there. */
    void forceOrientation(boolean landscape) {
        lockedOrientation = landscape ? 1 : 2;
        Log.i(TAG, "the PC does not follow the device orientation: staying " + (landscape ? "landscape" : "portrait"));
        OrientListener l = orientListener;
        if (l != null) l.onForceOrientation(landscape);
    }

    /** Any thread: the PC follows the device orientation (again): stop forcing one, the activity follows the sensor (or the user's setting) again. */
    void releaseOrientation() {
        lockedOrientation = 0;
        Log.i(TAG, "the PC follows the device orientation again: no longer locked");
        OrientListener l = orientListener;
        if (l != null) l.onReleaseOrientation();
    }

    /** The display rotation the display activity was last seen in (see the field): a new activity starts in it. */
    int lastRotation() {
        return lastRotation;
    }

    void setLastRotation(int rotation) {
        lastRotation = OrientationDetector.normalize(rotation);
    }

    /** The panel still shows the picture of the session that was lost (the display is in front and has content): a reconnect may send only changes. */
    boolean resumable() {
        return fb != null && renderer.hasContent();
    }

    View view() {
        Eink.Fb f = fb;
        return f != null ? f.view : null;
    }

    void showMessage(String text) {
        Eink.Fb f = fb;
        if (f == null) return;
        try {
            f.showMessage(text);
        } catch (Throwable t) {
            Log.w(TAG, "showMessage: " + t);
        }
    }

    static String idleMessage() {
        return "Padreen\n等待电脑…";
    }

    // ------------------------------------------------------------------------------------------------------------ state
    boolean active() {
        return active;
    }

    boolean reconnecting() {
        return reconnecting;
    }

    String kind() {
        return kind;
    }

    String label() {
        return label;
    }

    void addListener(Listener l) {
        listeners.addIfAbsent(l);
    }

    void removeListener(Listener l) {
        listeners.remove(l);
    }

    private void changed() {
        for (Listener l : listeners) l.onSessionChanged();
    }

    // ------------------------------------------------------------------------------------------------------------ session events
    /** Called by the receiver when a session starts (USB: the PC connected; LAN: the client connection is ready). */
    void sessionStarted(String kind, String label) {
        this.kind = kind;
        this.label = label;
        userDisconnect = false;
        reconnecting = false;
        lockedOrientation = 0;                // a new session may find a PC that follows the sensor
        active = true;
        SessionService.start(app, label + ("LAN".equals(kind) ? "（无线）" : "（USB）"));
        changed();
    }

    /**
     * The session is over.  byPc = the PC said BYE; lost = the link broke by itself (not BYE, not the user disconnecting): a LAN session
     * is then re-established in the background.
     */
    void sessionEnded(boolean byPc, boolean lost) {
        active = false;
        Handoff.ended(byPc);
        if ("LAN".equals(kind) && lost && !userDisconnect && lanPc != null) {
            startReconnect();
        } else {
            reconnecting = false;
            renderer.resetContent();
            SessionService.stop(app);
        }
        changed();
    }

    /** The user pressed "disconnect": end the session (or stop reconnecting) and do not connect again by itself. */
    void disconnect() {
        userDisconnect = true;
        Handoff.markUserEnded();
        boolean wasReconnecting = reconnecting;
        reconnecting = false;
        receiver.closeSession();
        if (!active && wasReconnecting) {
            renderer.resetContent();
            SessionService.stop(app);
            changed();
        }
    }

    /** The home screen connected to a PC and authenticated: run the display session on that connection. */
    void startLan(PcClient.Result r) {
        lanPc = r.pc;
        receiver.attachClient(r);
    }

    // ------------------------------------------------------------------------------------------------------------ reconnect (LAN)
    private void startReconnect() {
        final PcStore.Pc pc = lanPc;
        reconnecting = true;
        Log.i(TAG, "LAN session lost - reconnecting to " + pc.name);
        Thread th = new Thread(new Runnable() {
            @Override
            public void run() {
                long deadline = SystemClock.uptimeMillis() + 120000;
                String ip = pc.ip;
                int port = pc.port;
                while (reconnecting && SystemClock.uptimeMillis() < deadline) {
                    try {
                        Discovery.PcInfo info = Discovery.findOnce(app, pc.id, ip, 1500);   // the PC may have a new address
                        if (info != null) {
                            ip = info.ip;
                            port = info.port;
                            if (info.busy) {
                                Thread.sleep(2000);
                                continue;
                            }
                            PcClient.Result r = PcClient.connect(app, pc, ip, port);
                            new PcStore(app).markUsed(pc.id, ip, port);
                            if (!reconnecting) {                       // the user gave up meanwhile
                                try {
                                    r.socket.close();
                                } catch (IOException ignored) {
                                }
                                return;
                            }
                            Log.i(TAG, "reconnected to " + pc.name);
                            receiver.attachClient(r);                  // sessionStarted() clears `reconnecting`
                            return;
                        }
                    } catch (PcClient.Failure f) {
                        if (f.reason == PcClient.FAIL_UNPAIRED || f.reason == PcClient.FAIL_AUTH || f.reason == PcClient.FAIL_DISABLED) break;
                    } catch (IOException ignored) {
                    } catch (InterruptedException e) {
                        break;
                    }
                    try {
                        Thread.sleep(2000);
                    } catch (InterruptedException e) {
                        break;
                    }
                }
                if (reconnecting) {
                    reconnecting = false;
                    renderer.resetContent();
                    SessionService.stop(app);
                    changed();
                }
            }
        }, "pd-reconnect");
        th.start();
    }
}
