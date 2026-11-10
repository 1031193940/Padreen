package com.padreen.app;

import android.content.Context;
import android.util.Log;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.NetworkInterface;
import java.net.ServerSocket;
import java.net.Socket;
import java.util.Enumeration;
import java.util.zip.Inflater;

/**
 * Display-only receiver.  Two ways in, one session at a time:
 *  - USB:  the PC connects to 127.0.0.1:38700 through `adb forward`; plain frames, no authentication (only adb can reach it).
 *  - Wi-Fi: the PC connects to <tablet ip>:38701; mutual HMAC authentication with the key paired over USB, then AES-GCM records.
 * After the HELLO the tablet never sends anything the PC has to wait for; ACKs carry timestamps for latency measurement only.
 *
 * Frame: u8 type | u8 flags | u16 reserved | u32 length (little endian) | payload
 *   HELLO  0x01 tablet->PC  u16 proto, u16 w, u16 h, u16 dpi, u8 orientation, u8 appBuild, u16 rsv, u32 caps, u8[16] deviceId, u8[4] keyId
 *   UPDATE 0x10 PC->tablet  u32 seq, u8 mode, u8 codec, u16 x, u16 y, u16 w, u16 h, u32 dataLen, data
 *                           mode : 0 DU4, 1 DU, 2 GL16, 3 GC16 full, 4 DU4-5bit, 5 A2/MIX, 6..9 A2 with 6/7/8/9 frames
 *                           codec: 0 raw gray8, 1 zlib(gray8), 2 raw mono1 (rows byte aligned, MSB first, 1 = white), 3 LZ4 block(gray8),
 *                                  4 LZ4 block(mono1)
 *   ACK    0x12 tablet->PC  u32 seq, u64 tHeader, u64 tBody, u64 tDecoded, u64 tDrawn, u64 tPosted   (nanoTime)
 *   PING   0x20 PC->tablet  u64 pcTime            PONG 0x21 tablet->PC  u64 pcTime, u64 tabletNanoTime
 *   TOUCH  0x50 tablet->PC  u32 eventTimeMs, u8 actionMasked, u8 count, count x 16 bytes (see sendTouch)
 *   REFRESH 0x51 tablet->PC (empty) = the user asked for a full-screen refresh (three-finger tap): the PC answers with a fresh
 *                           full gray frame in mode 3 (GC16); without an answer in 1.5 s the tablet refreshes from its own shadow buffer
 *   ORIENT 0x54 tablet->PC  u16 w, u16 h, u8 seq, u8 rot, u16 rsv = the device was turned: please deliver a desktop of this size (see OrientSync)
 *   GEOM   0x11 PC->tablet  u16 w, u16 h, u8 seq, u8 flags (bit0 = refused, bit1 = NO_ROTATE), u16 rsv = the size the PC delivers now (answers
 *                           ORIENT with the same seq; seq 0 = announcement: start of a session, after a reconnect, the PC's rotation switch was
 *                           flipped).  NO_ROTATE = the PC does not follow the tablet's orientation right now: the desktop keeps this shape
 *                           whatever the tablet does, so the tablet locks to it until a later announcement comes without the flag.
 *   MEDIAKEY 0x55 tablet->PC u8 key (1 volume up, 2 volume down, 3 mute) = a hardware volume key while Padreen is in front and a PC is attached
 *   PAIR   0x70 PC->tablet  u8[32] key, u8[16] pcId, u8 nameLen, name (USB connection only)     PAIR_OK 0x75 tablet->PC  u8[4] keyId
 *   BYE    0x7F PC->tablet  (the PC quit: the session ends, the display closes)
 */
final class Receiver implements Runnable {
    static final String TAG = "Padreen";
    static final int PORT = 38700;          // loopback (USB)

    static final int T_HELLO = 0x01, T_UPDATE = 0x10, T_GEOM = 0x11, T_ACK = 0x12, T_PING = 0x20, T_PONG = 0x21, T_VERIFY = 0x30,
            T_VERIFYR = 0x31, T_SELFTEST = 0x40, T_SELFTESTR = 0x41, T_TOUCH = 0x50, T_REFRESH = 0x51, T_LEAVE = 0x52, T_GESTURE = 0x53,
            T_ORIENT = 0x54, T_MEDIAKEY = 0x55, T_MARK = 0x60, T_WAKE = 0x61, T_WAKER = 0x62,
            T_RAWWAVE = 0x63, T_PAIR = 0x70, T_CHALLENGE = 0x71, T_AUTH = 0x72, T_AUTH_OK = 0x73, T_AUTH_FAIL = 0x74, T_PAIR_OK = 0x75,
            T_BYE = 0x7F;
    static final int CAP_ZLIB = 1, CAP_MONO1 = 2, CAP_LZ4 = 4, CAP_LZ4_MONO = 8, CAP_TOUCH = 16, CAP_RESUME = 32, CAP_ORIENT = 64, CAP_MEDIAKEYS = 128;
    /** Bumped whenever the PC host needs a newer tablet app; reported in HELLO byte 9 (the host installs the bundled APK if lower). Keep in sync with versionCode in AndroidManifest.xml. */
    static final int APP_BUILD = 18;

    private final Context ctx;
    private final DisplayHub hub;
    private final Renderer renderer;
    private OrientSync sync;                        // set in the constructor (before any thread runs)
    private volatile boolean running = true;
    private ServerSocket server;
    private final Object sessionLock = new Object();

    // ---- touch feedback (tablet -> PC) -------------------------------------------------------------------------------------
    /** The channel of the running session (null between sessions); touch frames are written to it from {@link #touchThread}. */
    private volatile Chan activeChan;
    /** Outgoing frames of the UI side: {Integer type, byte[] payload}; written by pd-touch so the UI thread never touches the socket. */
    private final java.util.concurrent.LinkedBlockingQueue<Object[]> touchQ = new java.util.concurrent.LinkedBlockingQueue<Object[]>(256);
    private Thread touchThread;
    private final android.os.Handler uiHandler = new android.os.Handler(android.os.Looper.getMainLooper());
    private long lastFullReq;                       // UI thread only
    private volatile int fullFrames;                // full-panel GC16 updates received from the PC (any thread)
    private int touchMask;                          // pointer ids the PC currently holds down (UI thread only)
    private final int[] lastX = new int[16], lastY = new int[16];

    /**
     * Called on the UI thread for every touch event of the activity.  One frame per event with ALL pointers:
     *   u32 eventTimeMs | u8 actionMasked | u8 count | count x { u8 id, u8 state (0 move, 1 down, 2 up, 3 cancel), u8 tool (0 finger,
     *   1 stylus, 2 eraser, 3 other), u8 buttons, u16 x, u16 y (panel pixels), u16 pressure (0..1000), u16 touchMajor, u16 touchMinor, u16 rsv }
     * Writing to the socket happens on a separate thread so a stalled network can never block the UI thread.
     */
    void sendTouch(android.view.MotionEvent e) {
        if (activeChan == null || sync.pending()) return;      // while the device is being turned the PC's monitor has another shape: no touches
        final int W = renderer.width(), H = renderer.height();
        int n = Math.min(e.getPointerCount(), 10);
        int am = e.getActionMasked(), ai = e.getActionIndex();
        float sx = 1f, sy = 1f;
        android.view.View view = hub.view();
        if (view != null && view.getWidth() > 0 && view.getHeight() > 0) {
            sx = (float) W / view.getWidth();
            sy = (float) H / view.getHeight();
        }
        byte[] p = new byte[6 + n * 16];
        put32(p, 0, (int) e.getEventTime());
        p[4] = (byte) am;
        p[5] = (byte) n;
        boolean onlyMoves = true;
        for (int i = 0; i < n; i++) {
            int o = 6 + i * 16;
            int st = 0;
            switch (am) {
                case android.view.MotionEvent.ACTION_DOWN:
                    st = 1;
                    break;
                case android.view.MotionEvent.ACTION_POINTER_DOWN:
                    if (i == ai) st = 1;
                    break;
                case android.view.MotionEvent.ACTION_UP:
                    st = 2;
                    break;
                case android.view.MotionEvent.ACTION_POINTER_UP:
                    if (i == ai) st = 2;
                    break;
                case android.view.MotionEvent.ACTION_CANCEL:
                    st = 3;
                    break;
                default:
                    break;
            }
            if (st != 0) onlyMoves = false;
            int tool;
            switch (e.getToolType(i)) {
                case android.view.MotionEvent.TOOL_TYPE_FINGER:
                    tool = 0;
                    break;
                case android.view.MotionEvent.TOOL_TYPE_STYLUS:
                    tool = 1;
                    break;
                case android.view.MotionEvent.TOOL_TYPE_ERASER:
                    tool = 2;
                    break;
                default:
                    tool = 3;
            }
            int pid = e.getPointerId(i) & 15;
            int px = clamp((int) (e.getX(i) * sx), 0, W - 1), py = clamp((int) (e.getY(i) * sy), 0, H - 1);
            lastX[pid] = px;
            lastY[pid] = py;
            if (st == 1) touchMask |= 1 << pid;
            else if (st == 2 || st == 3) touchMask &= ~(1 << pid);
            p[o] = (byte) e.getPointerId(i);
            p[o + 1] = (byte) st;
            p[o + 2] = (byte) tool;
            p[o + 3] = (byte) e.getButtonState();
            put16(p, o + 4, px);
            put16(p, o + 6, py);
            put16(p, o + 8, clamp((int) (e.getPressure(i) * 1000f), 0, 1000));
            put16(p, o + 10, clamp((int) (e.getTouchMajor(i) * sx), 0, 65535));
            put16(p, o + 12, clamp((int) (e.getTouchMinor(i) * sx), 0, 65535));
        }
        // a flood of pure moves while the network is slow: the newest position supersedes the older ones
        if (onlyMoves && touchQ.size() > 64) return;
        enqueue(T_TOUCH, p);
    }

    private void enqueue(int type, byte[] p) {
        Object[] it = new Object[]{Integer.valueOf(type), p};
        if (!touchQ.offer(it)) {
            touchQ.poll();
            touchQ.offer(it);
        }
    }

    /** A three-finger swipe (1 = up: multiple desktops, 2 = down: window switcher): the PC turns it into the matching keyboard shortcut. UI thread. */
    void sendGesture(int kind) {
        if (activeChan == null) return;
        Log.i(TAG, "gesture sent: " + (kind == 1 ? "swipe up" : "swipe down"));
        enqueue(T_GESTURE, new byte[]{(byte) kind});
    }

    private final VolumeKeys volumeKeys = new VolumeKeys();      // UI thread only

    /**
     * A hardware volume key reached one of Padreen's windows (the display, the home screen or a dialog on it; they all forward their key events
     * here): while the user's switch is on and a PC is attached - a running session, or its Wi-Fi link is being re-established - the key belongs
     * to the PC.  Every event of it is swallowed, so the tablet's own volume (and its volume panel) never reacts; a key press is sent to the PC,
     * which turns it into a Windows volume key (holding the key repeats about ten steps a second; while the link is re-established the key is
     * only swallowed).  With no PC at all, or the switch off, nothing is taken and the system handles the key as usual.  The switch is read for
     * every event, so flipping it on the home screen takes effect at once.  UI thread; no system setting is touched.
     * @return true = the event was taken (the window must swallow it), false = the system handles it
     */
    boolean handleVolumeKey(android.view.KeyEvent ev) {
        int kind;
        switch (ev.getKeyCode()) {
            case android.view.KeyEvent.KEYCODE_VOLUME_UP:
                kind = 1;
                break;
            case android.view.KeyEvent.KEYCODE_VOLUME_DOWN:
                kind = 2;
                break;
            case android.view.KeyEvent.KEYCODE_VOLUME_MUTE:
                kind = 3;
                break;
            default:
                return false;
        }
        boolean live = activeChan != null;                       // a session channel exists: a key can really be sent right now
        int what = volumeKeys.decide(new PcStore(ctx).volumeToPc(), live || hub.active() || hub.reconnecting(), live,
                ev.getAction() == android.view.KeyEvent.ACTION_DOWN, ev.getEventTime());
        if (what == VolumeKeys.SEND) {
            Log.i(TAG, "volume key -> PC: " + (kind == 1 ? "up" : kind == 2 ? "down" : "mute"));
            enqueue(T_MEDIAKEY, new byte[]{(byte) kind});
        }
        return what != VolumeKeys.PASS;
    }

    /** The display surface has this logical size (portrait / landscape): the PC has to deliver a picture of that shape.  UI thread. */
    void onSurfaceSize(int w, int h) {
        sync.onSize(w, h);
    }

    /** Lift every finger the PC thinks is still down (the activity goes away mid-touch). UI thread. */
    void cancelTouches() {
        if (activeChan == null || touchMask == 0) return;
        int n = 0;
        for (int id = 0; id < 16; id++) if ((touchMask & (1 << id)) != 0) n++;
        byte[] p = new byte[6 + n * 16];
        put32(p, 0, (int) android.os.SystemClock.uptimeMillis());
        p[4] = (byte) android.view.MotionEvent.ACTION_CANCEL;
        p[5] = (byte) n;
        int k = 0;
        for (int id = 0; id < 16; id++) {
            if ((touchMask & (1 << id)) == 0) continue;
            int o = 6 + k++ * 16;
            p[o] = (byte) id;
            p[o + 1] = 3;
            put16(p, o + 4, lastX[id]);
            put16(p, o + 6, lastY[id]);
            put16(p, o + 8, 500);
        }
        touchMask = 0;
        enqueue(T_TOUCH, p);
    }

    /**
     * One-key full-screen refresh (three-finger tap).  Asks the PC for a fresh full gray frame (GC16); if none arrives within
     * 1.5 s (old host, stalled link, nobody connected) the tablet redraws its own shadow buffer in GC16.  UI thread.
     */
    void requestFullRefresh(String why) {
        long now = android.os.SystemClock.uptimeMillis();
        if (now - lastFullReq < 1500) return;
        lastFullReq = now;
        Log.i(TAG, "full refresh requested (" + why + ")");
        if (activeChan == null) {
            renderer.requestFull();
            return;
        }
        final int before = fullFrames;
        enqueue(T_REFRESH, new byte[0]);
        uiHandler.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (fullFrames == before) {
                    Log.i(TAG, "the PC did not answer - refreshing from the tablet's own buffer");
                    renderer.requestFull();
                }
            }
        }, 1500);
    }

    private static int clamp(int v, int lo, int hi) {
        return v < lo ? lo : v > hi ? hi : v;
    }

    private void startTouchThread() {
        if (touchThread != null) return;
        touchThread = new Thread(new Runnable() {
            @Override
            public void run() {
                while (running) {
                    try {
                        Object[] it = touchQ.take();
                        Chan ch = activeChan;
                        byte[] p = (byte[]) it[1];
                        if (ch != null) ch.write(((Integer) it[0]).intValue(), p, p.length);
                    } catch (InterruptedException ie) {
                        return;
                    } catch (IOException ignored) {
                        // the session thread notices the broken connection by itself
                    }
                }
            }
        }, "pd-touch");
        touchThread.setPriority(Thread.MAX_PRIORITY);
        touchThread.start();
    }

    Receiver(Context ctx, DisplayHub hub, Renderer renderer) {
        this.ctx = ctx;
        this.hub = hub;
        this.renderer = renderer;
        this.sync = new OrientSync(new OrientSync.Sink() {
            @Override
            public void sendOrient(int w, int h, int seq) {
                Log.i(TAG, "device turned: asking the PC for a " + w + "x" + h + " desktop (#" + seq + ")");
                byte[] p = new byte[8];
                put16(p, 0, w);
                put16(p, 2, h);
                p[4] = (byte) seq;
                enqueue(T_ORIENT, p);
            }

            @Override
            public void forceOrientation(boolean landscape) {
                Receiver.this.hub.forceOrientation(landscape);
            }

            @Override
            public void releaseOrientation() {
                Receiver.this.hub.releaseOrientation();
            }

            @Override
            public void armTimeout(final int seq) {
                uiHandler.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        sync.timeout(seq);
                    }
                }, 3000);
            }

            @Override
            public void requestFullFrame() {
                uiHandler.post(new Runnable() {
                    @Override
                    public void run() {
                        requestFullRefresh("display turned");
                    }
                });
            }
        });
    }
    void stop() {
        running = false;
        closeQuietly(clientSocket);                       // ends a running LAN session (its read blocks on this socket)
        if (touchThread != null) touchThread.interrupt();
        try {
            if (server != null) server.close();
        } catch (IOException ignored) {
        }
    }

    private void showWaiting(String first) {
        hub.showMessage("Padreen\n" + first);
    }

    /** USB path: the PC connects to 127.0.0.1:38700 through `adb forward` (started by Padreen when the cable is plugged in). */
    @Override
    public void run() {
        try {
            server = new ServerSocket();
            server.setReuseAddress(true);
            server.bind(new InetSocketAddress(InetAddress.getByName("127.0.0.1"), PORT));
        } catch (IOException e) {
            Log.e(TAG, "cannot listen on " + PORT, e);
            hub.showMessage("Padreen\n端口 " + PORT + " 被占用\n" + e.getMessage());
            return;
        }
        Log.i(TAG, "listening on 127.0.0.1:" + PORT);
        startTouchThread();
        while (running) {
            Socket s = null;
            try {
                s = server.accept();
                s.setTcpNoDelay(true);
                Log.i(TAG, "PC connected (USB)");
                serveUsb(s);
            } catch (IOException e) {
                if (running) Log.w(TAG, "connection ended: " + e);
            } catch (Throwable t) {
                Log.e(TAG, "serve failed", t);
            } finally {
                closeQuietly(s);
            }
        }
    }

    private volatile boolean clientMode;
    private volatile Socket clientSocket;

    /**
     * LAN path: the home screen connected to a PC and authenticated (PD2); run the display session on that channel.  The hub is told when
     * the session starts and ends (and whether the link was lost, which makes it reconnect in the background).
     */
    void attachClient(final PcClient.Result r) {
        clientMode = true;
        startTouchThread();
        Thread t = new Thread(new Runnable() {
            @Override
            public void run() {
                boolean byPc = false;
                boolean lost = true;
                sessionSocket = r.socket;
                try {
                    synchronized (sessionLock) {
                        hub.sessionStarted("LAN", r.pc.name);
                        try {
                            serve(r.chan, true);
                            byPc = byeSeen;
                            lost = !byPc;
                        } finally {
                            activeChan = null;
                            sync.sessionEnded();
                            touchQ.clear();
                            renderer.setSink(null);
                        }
                    }
                } catch (IOException e) {
                    Log.w(TAG, "LAN session ended: " + e);
                } catch (Throwable th) {
                    Log.e(TAG, "LAN session failed", th);
                } finally {
                    closeQuietly(r.socket);
                    sessionSocket = null;
                    hub.sessionEnded(byPc, lost && !byPc);
                }
            }
        }, "pd-client");
        t.setPriority(Thread.MAX_PRIORITY);
        t.start();
    }

    private volatile boolean byeSeen;
    private volatile Socket sessionSocket;

    /**
     * The user disconnects on purpose: tell the PC (T_LEAVE: it ends the session at once instead of waiting for us to reconnect), then close
     * the running session's connection (its read loop ends).
     */
    void closeSession() {
        final Chan ch = activeChan;
        final Socket s = sessionSocket;
        if (ch == null) {
            closeQuietly(s);
            return;
        }
        new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    ch.write(T_LEAVE, new byte[0], 0);
                    Thread.sleep(150);                  // let it leave the socket buffer
                } catch (Exception ignored) {
                }
                closeQuietly(s);
            }
        }, "pd-leave").start();
    }

    private static void closeQuietly(Socket s) {
        if (s != null) {
            try {
                s.close();
            } catch (IOException ignored) {
            }
        }
    }

    private void serveUsb(Socket s) throws IOException {
        Chan ch = new Chan.Plain(s.getInputStream(), s.getOutputStream());
        sessionSocket = s;
        boolean started = false;
        synchronized (sessionLock) {
            try {
                hub.sessionStarted("USB", "电脑");
                started = true;
                serve(ch, true);
            } finally {
                activeChan = null;
                sync.sessionEnded();
                touchQ.clear();
                renderer.setSink(null);
                sessionSocket = null;
                if (started) hub.sessionEnded(byeSeen, false);
            }
        }
        if (running) showWaiting("已断开，等待重连…");
    }
    /**
     * The data-plane HELLO (first record of every session): u16 proto, u16 w, u16 h, u16 dpi, u8 orientation, u8 appBuild, u16 rsv, u32 caps,
     * u8[16] deviceId, u8[4] keyId (unused now), u8 nameLen, name, u8 nKeys, nKeys x u8[4] (key ids of every PC we are paired with).
     */
    private byte[] buildHello(int w, int h) {
        byte[] name = Secure.deviceName(ctx).getBytes(java.nio.charset.Charset.forName("UTF-8"));
        java.util.List<byte[]> ids = new PcStore(ctx).keyIds();
        byte[] hello = new byte[37 + name.length + 1 + 4 * ids.size()];
        put16(hello, 0, 2);
        put16(hello, 2, w);
        put16(hello, 4, h);
        put16(hello, 6, 227);
        hello[8] = (byte) (w >= h ? 1 : 2);                // 1 = landscape, 2 = portrait (the desktop is created in this shape)
        hello[9] = (byte) APP_BUILD;
        put32(hello, 12, CAP_ZLIB | CAP_MONO1 | CAP_LZ4 | CAP_LZ4_MONO | CAP_TOUCH | CAP_ORIENT | CAP_MEDIAKEYS
                | (hub.resumable() ? CAP_RESUME : 0));      // RESUME: this panel still shows the PC's last picture
        System.arraycopy(Secure.deviceId(ctx), 0, hello, 16, 16);
        hello[36] = (byte) name.length;
        System.arraycopy(name, 0, hello, 37, name.length);
        int o = 37 + name.length;
        hello[o++] = (byte) ids.size();
        for (byte[] id : ids) {
            System.arraycopy(id, 0, hello, o, 4);
            o += 4;
        }
        return hello;
    }

    private static int le32(byte[] b, int o) {
        return Chan.le32(b, o);
    }

    private static int le16(byte[] b, int o) {
        return (b[o] & 0xff) | ((b[o + 1] & 0xff) << 8);
    }

    private static void put32(byte[] b, int o, int v) {
        Chan.put32(b, o, v);
    }

    private static void put64(byte[] b, int o, long v) {
        put32(b, o, (int) v);
        put32(b, o + 4, (int) (v >>> 32));
    }

    private static void put16(byte[] b, int o, int v) {
        b[o] = (byte) v;
        b[o + 1] = (byte) (v >> 8);
    }

    private void serve(final Chan ch, boolean sendHello) throws IOException {
        // both shapes of the display (1872x1404 and 1404x1872) have the same number of pixels: the buffers fit either way
        final int maxPix = renderer.width() * renderer.height();
        byte[] hdr = new byte[8];
        byte[] body = new byte[maxPix + 4096];
        byte[] gray = new byte[maxPix];
        byte[] packed = new byte[maxPix / 8 + 4096];                 // 1 bit per pixel with every row padded to a byte: < maxPix/8 + height
        Inflater inflater = new Inflater();
        renderer.setSink(new Renderer.AckSink() {
            @Override
            public void ack(int seq, long tHdr, long tBody, long tDecoded, long tDrawn, long tPosted) {
                byte[] a = new byte[44];
                put32(a, 0, seq);
                put64(a, 4, tHdr);
                put64(a, 12, tBody);
                put64(a, 20, tDecoded);
                put64(a, 28, tDrawn);
                put64(a, 36, tPosted);
                try {
                    ch.write(T_ACK, a, a.length);
                } catch (IOException ignored) {
                }
            }
        });

        final int helloW = renderer.width(), helloH = renderer.height();
        if (sendHello) {
            byte[] hello = buildHello(helloW, helloH);
            ch.write(T_HELLO, hello, hello.length);
        }

        touchQ.clear();
        touchMask = 0;
        byeSeen = false;
        sync.sessionStarted(helloW, helloH);
        activeChan = ch;                       // from now on touch frames may flow to the PC
        if (renderer.width() != helloW || renderer.height() != helloH) sync.onSize(renderer.width(), renderer.height());   // turned while the HELLO went out
        while (running) {
            int len = ch.readFrame(hdr, body);
            long tHdr = System.nanoTime();
            long tBody = tHdr;
            int type = hdr[0] & 0xff;
            switch (type) {
                case T_UPDATE:
                    try {
                        handleUpdate(body, len, gray, packed, inflater, tHdr, tBody);
                    } catch (IOException e) {
                        throw e;
                    } catch (Throwable t) {
                        Log.e(TAG, "update failed", t);
                    }
                    break;
                case T_PING: {
                    byte[] pong = new byte[16];
                    System.arraycopy(body, 0, pong, 0, 8);
                    put64(pong, 8, System.nanoTime());
                    ch.write(T_PONG, pong, pong.length);
                    break;
                }
                case T_PAIR:
                    // USB (physical trust) pairing: u8[32] K | u8[16] pcId | u8 nameLen | pcName   (only over the plain loopback connection)
                    if (ch instanceof Chan.Plain && len >= 49 && 49 + (body[48] & 0xff) <= len) {
                        byte[] key = new byte[32], pcId = new byte[16];
                        System.arraycopy(body, 0, key, 0, 32);
                        System.arraycopy(body, 32, pcId, 0, 16);
                        String pcName = new String(body, 49, body[48] & 0xff, java.nio.charset.Charset.forName("UTF-8"));
                        PcStore store = new PcStore(ctx);
                        PcStore.Pc pc = store.find(pcId);
                        if (pc == null) pc = new PcStore.Pc();
                        pc.id = pcId;
                        pc.name = pcName;
                        pc.key = key;
                        pc.lastUsed = System.currentTimeMillis();
                        store.put(pc);
                        Log.i(TAG, "paired with PC '" + pcName + "' over USB");
                        byte[] id = Secure.keyId(key);
                        ch.write(T_PAIR_OK, id, id.length);
                    } else {
                        Log.w(TAG, "PAIR ignored (secure channel or bad length)");
                    }
                    break;
                case T_GEOM:
                    // PC -> tablet: u16 w, u16 h, u8 seq, u8 flags (bit0 = the PC could not switch, bit1 = NO_ROTATE: it keeps this shape), u16 rsv
                    if (len >= 6) {
                        int gw = le16(body, 0), gh = le16(body, 2), gseq = body[4] & 0xff, gflags = body[5] & 0xff;
                        boolean refusal = (gflags & 1) != 0, noRotate = (gflags & 2) != 0;
                        Log.i(TAG, "PC delivers " + gw + "x" + gh + " (" + (gseq == 0 ? "announcement" : "answer #" + gseq) + (refusal ? ", could not switch" : "")
                                + (noRotate ? ", does not follow the tablet's orientation" : "") + ")");
                        sync.onGeom(gw, gh, gseq, refusal, noRotate);
                    }
                    break;
                case T_RAWWAVE:       // debug: i32 raw EinkMode for every following post (-1 = back to protocol modes)
                    Renderer.rawWave = le32(body, 0);
                    Log.i(TAG, "RAWWAVE " + Renderer.rawWave);
                    break;
                case T_MARK:
                    Log.i(TAG, "MARK " + new String(body, 0, len, "UTF-8"));
                    break;
                case T_BYE:
                    byeSeen = true;
                    Log.i(TAG, "BYE from PC");
                    return;
                default:
                    Log.w(TAG, "ignoring frame type " + type);
            }
        }
    }

    private static void expandMono(byte[] src, int srcOff, int w, int h, byte[] gray) {
        int stride = (w + 7) >> 3;
        int o = 0;
        for (int row = 0; row < h; row++) {
            int ro = srcOff + row * stride;
            for (int col = 0; col < w; col++) {
                gray[o++] = (byte) (((src[ro + (col >> 3)] >> (7 - (col & 7))) & 1) != 0 ? 0xFF : 0x00);
            }
        }
    }

    private void handleUpdate(byte[] b, int len, byte[] gray, byte[] packed, Inflater inflater, long tHdr, long tBody) throws Exception {
        if (sync.pending()) return;               // the device was turned: whatever is still on the wire has the old shape (T_GEOM ends the wait)
        final int W = renderer.width(), H = renderer.height();
        int seq = le32(b, 0);
        int mode = b[4] & 0xff;
        int codec = b[5] & 0xff;
        int x = le16(b, 6), y = le16(b, 8), w = le16(b, 10), h = le16(b, 12);
        int dlen = le32(b, 14);
        final int off = 18;
        final int n = w * h;
        if (w <= 0 || h <= 0 || x + w > W || y + h > H || off + dlen > len) {
            Log.w(TAG, "bad update rect " + x + "," + y + " " + w + "x" + h);
            return;
        }
        byte[] src;
        int srcOff;
        if (codec == 0) {
            src = b;
            srcOff = off;
        } else if (codec == 1) {
            inflater.reset();
            inflater.setInput(b, off, dlen);
            int got = 0;
            while (got < n) {
                int r = inflater.inflate(gray, got, n - got);
                if (r == 0 && (inflater.finished() || inflater.needsInput())) break;
                got += r;
            }
            if (got != n) {
                Log.w(TAG, "zlib size mismatch " + got + " != " + n);
                return;
            }
            src = gray;
            srcOff = 0;
        } else if (codec == 2) {
            expandMono(b, off, w, h, gray);
            src = gray;
            srcOff = 0;
        } else if (codec == 3) {
            int got = Lz4.decompress(b, off, dlen, gray, 0, n);
            if (got != n) {
                Log.w(TAG, "lz4 size mismatch " + got + " != " + n);
                return;
            }
            src = gray;
            srcOff = 0;
        } else if (codec == 4) {
            int want = ((w + 7) >> 3) * h;
            int got = Lz4.decompress(b, off, dlen, packed, 0, want);
            if (got != want) {
                Log.w(TAG, "lz4 mono size mismatch " + got + " != " + want);
                return;
            }
            expandMono(packed, 0, w, h, gray);
            src = gray;
            srcOff = 0;
        } else {
            Log.w(TAG, "unknown codec " + codec);
            return;
        }
        long tDecoded = System.nanoTime();
        if (mode == 3 && (long) w * h >= (long) W * H * 9 / 10) fullFrames++;      // the PC's answer to a refresh request
        // the render thread paces the display and sends the ACK once the pixels were actually posted
        renderer.submit(mode, x, y, w, h, src, srcOff, seq, tHdr, tBody, tDecoded);
    }
}
