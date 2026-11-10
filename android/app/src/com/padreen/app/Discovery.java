package com.padreen.app;

import android.util.Log;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InterfaceAddress;
import java.net.NetworkInterface;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.List;

/**
 * Finds Padreen PCs on the local network: UDP query "PDQ2"+padId to the broadcast addresses (and to manually added hosts) every
 * 1.5 s, replies "PDR2" come back as unicast.  Runs in its own thread while the home screen is visible.
 */
final class Discovery {
    static final String TAG = "Padreen";
    static final int PORT = 38703;

    static final class PcInfo {
        byte[] id;
        String name;
        String ip;
        int port;
        boolean pairing;     // the PC's pairing window is open
        boolean busy;        // the PC already serves a session
        long seen;           // uptimeMillis of the last reply
    }

    interface Listener {
        void onPc(PcInfo pc);
    }

    private final byte[] query;
    private final Listener listener;
    private volatile boolean running;
    private Thread thread;
    private DatagramSocket socket;
    private final List<String> extraHosts = new ArrayList<String>();

    Discovery(byte[] padId, Listener l) {
        this.listener = l;
        this.query = Secure.cat(Secure.ascii("PDQ2"), padId);
    }

    synchronized void addHost(String ip) {
        if (!extraHosts.contains(ip)) extraHosts.add(ip);
    }

    synchronized void start() {
        if (running) return;
        running = true;
        thread = new Thread(new Runnable() {
            @Override
            public void run() {
                loop();
            }
        }, "pd-discovery");
        thread.start();
    }

    synchronized void stop() {
        running = false;
        if (socket != null) socket.close();
    }

    private List<InetAddress> targets() {
        List<InetAddress> out = new ArrayList<InetAddress>();
        try {
            out.add(InetAddress.getByName("255.255.255.255"));
            Enumeration<NetworkInterface> e = NetworkInterface.getNetworkInterfaces();
            while (e != null && e.hasMoreElements()) {
                NetworkInterface ni = e.nextElement();
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InterfaceAddress ia : ni.getInterfaceAddresses()) {
                    InetAddress b = ia.getBroadcast();
                    if (b != null && !out.contains(b)) out.add(b);
                }
            }
        } catch (Exception ignored) {
        }
        synchronized (this) {
            for (String h : extraHosts) {
                try {
                    out.add(InetAddress.getByName(h));
                } catch (Exception ignored) {
                }
            }
        }
        return out;
    }

    private void loop() {
        try {
            socket = new DatagramSocket();
            socket.setBroadcast(true);
            socket.setSoTimeout(250);
        } catch (Exception e) {
            Log.w(TAG, "discovery socket failed: " + e);
            running = false;
            return;
        }
        long nextSend = 0;
        byte[] buf = new byte[512];
        while (running) {
            long now = android.os.SystemClock.uptimeMillis();
            if (now >= nextSend) {
                nextSend = now + 1500;
                for (InetAddress t : targets()) {
                    try {
                        socket.send(new DatagramPacket(query, query.length, t, PORT));
                    } catch (Exception ignored) {
                    }
                }
            }
            try {
                DatagramPacket p = new DatagramPacket(buf, buf.length);
                socket.receive(p);
                PcInfo info = parse(p);
                if (info != null && listener != null) listener.onPc(info);
            } catch (java.net.SocketTimeoutException ignored) {
            } catch (Exception e) {
                if (running) Log.w(TAG, "discovery receive: " + e);
            }
        }
        try {
            socket.close();
        } catch (Exception ignored) {
        }
    }

    /**
     * One-shot search for a specific PC (used when a LAN session broke and is being re-established): unicast to its last address plus the
     * usual broadcasts, wait up to timeoutMs for its reply.  Returns null if it did not answer.
     */
    static PcInfo findOnce(android.content.Context c, byte[] pcId, String lastIp, int timeoutMs) {
        Discovery d = new Discovery(Secure.deviceId(c), null);
        if (lastIp != null && !lastIp.isEmpty()) d.addHost(lastIp);
        DatagramSocket s = null;
        try {
            s = new DatagramSocket();
            s.setBroadcast(true);
            s.setSoTimeout(250);
            for (InetAddress t : d.targets()) {
                try {
                    s.send(new DatagramPacket(d.query, d.query.length, t, PORT));
                } catch (Exception ignored) {
                }
            }
            long end = android.os.SystemClock.uptimeMillis() + timeoutMs;
            byte[] buf = new byte[512];
            while (android.os.SystemClock.uptimeMillis() < end) {
                try {
                    DatagramPacket p = new DatagramPacket(buf, buf.length);
                    s.receive(p);
                    PcInfo i = parse(p);
                    if (i != null && java.util.Arrays.equals(i.id, pcId)) return i;
                } catch (java.net.SocketTimeoutException ignored) {
                }
            }
        } catch (Exception ignored) {
        } finally {
            if (s != null) s.close();
        }
        return null;
    }
    /** "PDR2" | ver | flags | u16 tcpPort | id[16] | nameLen | name */
    private static PcInfo parse(DatagramPacket p) {
        byte[] d = p.getData();
        int n = p.getLength();
        if (n < 26 || d[0] != 'P' || d[1] != 'D' || d[2] != 'R' || d[3] != '2' || d[4] != 2) return null;
        int flags = d[5] & 0xff;
        int port = (d[6] & 0xff) | ((d[7] & 0xff) << 8);
        byte[] id = new byte[16];
        System.arraycopy(d, 8, id, 0, 16);
        int nl = d[24] & 0xff;
        if (25 + nl > n) return null;
        PcInfo i = new PcInfo();
        i.id = id;
        i.name = new String(d, 25, nl, java.nio.charset.Charset.forName("UTF-8"));
        i.ip = p.getAddress().getHostAddress();
        i.port = port;
        i.pairing = (flags & 1) != 0;
        i.busy = (flags & 2) != 0;
        i.seen = android.os.SystemClock.uptimeMillis();
        return i;
    }
}
