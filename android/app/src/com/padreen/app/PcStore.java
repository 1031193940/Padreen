package com.padreen.app;

import android.content.Context;
import android.content.SharedPreferences;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * The computers this tablet is paired with (several are fine: home PC, office PC ...).  Each entry holds the 32-byte pairing key K that
 * both sides derived during pairing (over the LAN with a code, or over USB).  Kept in this app's private preferences.
 */
final class PcStore {
    static final class Pc {
        byte[] id;           // 16 bytes
        String name;
        byte[] key;          // 32 bytes
        String ip;           // last known address
        int port = 38702;
        long lastUsed;

        String idHex() {
            return Secure.toHex(id);
        }
    }

    private static final String PREFS = "pcs";
    private final SharedPreferences prefs;

    PcStore(Context c) {
        prefs = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }

    synchronized List<Pc> list() {
        List<Pc> out = new ArrayList<Pc>();
        try {
            JSONArray a = new JSONArray(prefs.getString("list", "[]"));
            for (int i = 0; i < a.length(); i++) {
                JSONObject o = a.getJSONObject(i);
                Pc p = new Pc();
                p.id = Secure.fromHex(o.getString("id"));
                p.name = o.optString("name", "PC");
                p.key = Secure.fromHex(o.getString("key"));
                p.ip = o.optString("ip", "");
                p.port = o.optInt("port", 38702);
                p.lastUsed = o.optLong("last", 0);
                if (p.id.length == 16 && p.key.length == 32) out.add(p);
            }
        } catch (Exception ignored) {
        }
        return out;
    }

    synchronized Pc find(byte[] id) {
        for (Pc p : list()) if (java.util.Arrays.equals(p.id, id)) return p;
        return null;
    }

    synchronized void put(Pc pc) {
        List<Pc> all = list();
        boolean found = false;
        for (int i = 0; i < all.size(); i++) {
            if (java.util.Arrays.equals(all.get(i).id, pc.id)) {
                all.set(i, pc);
                found = true;
            }
        }
        if (!found) all.add(pc);
        save(all);
    }

    synchronized void remove(byte[] id) {
        List<Pc> all = list();
        for (int i = all.size() - 1; i >= 0; i--) if (java.util.Arrays.equals(all.get(i).id, id)) all.remove(i);
        save(all);
    }

    synchronized void markUsed(byte[] id, String ip, int port) {
        Pc p = find(id);
        if (p == null) return;
        p.lastUsed = System.currentTimeMillis();
        if (ip != null) p.ip = ip;
        p.port = port;
        put(p);
    }

    /** The computer used most recently (auto-connect target), or null. */
    synchronized Pc last() {
        Pc best = null;
        for (Pc p : list()) if (best == null || p.lastUsed > best.lastUsed) best = p;
        return best;
    }

    /** Key ids of every stored key - listed in the USB HELLO so a PC can tell whether it still needs to pair. */
    synchronized List<byte[]> keyIds() {
        List<byte[]> out = new ArrayList<byte[]>();
        for (Pc p : list()) out.add(Secure.keyId(p.key));
        return out;
    }

    /** The three-finger gestures (tap = refresh, swipe up / down = desktops / window switcher); they cost the first touch ~60 ms (see TouchGate). */
    boolean gestures() {
        return prefs.getBoolean("gestures", true);
    }

    void setGestures(boolean on) {
        prefs.edit().putBoolean("gestures", on).apply();
    }

    /** Follow the device's orientation: turned to portrait, the PC's virtual monitor for this tablet becomes portrait too (and back) - unless the PC does not follow (see OrientSync). */
    boolean autoRotate() {
        return prefs.getBoolean("rotate", true);
    }

    void setAutoRotate(boolean on) {
        prefs.edit().putBoolean("rotate", on).apply();
    }

    /**
     * While Padreen is in front and a PC is attached, the hardware volume keys change the PC's volume and never the tablet's (otherwise they
     * change the tablet's own).  Read for every key event (see {@link Receiver#handleVolumeKey}), never cached.
     */
    boolean volumeToPc() {
        return prefs.getBoolean("volume_pc", true);
    }

    void setVolumeToPc(boolean on) {
        prefs.edit().putBoolean("volume_pc", on).apply();
    }

    boolean autoConnect() {
        return prefs.getBoolean("auto", true);
    }

    void setAutoConnect(boolean on) {
        prefs.edit().putBoolean("auto", on).commit();
    }

    private void save(List<Pc> all) {
        try {
            JSONArray a = new JSONArray();
            for (Pc p : all) {
                JSONObject o = new JSONObject();
                o.put("id", Secure.toHex(p.id));
                o.put("name", p.name);
                o.put("key", Secure.toHex(p.key));
                o.put("ip", p.ip == null ? "" : p.ip);
                o.put("port", p.port);
                o.put("last", p.lastUsed);
                a.put(o);
            }
            prefs.edit().putString("list", a.toString()).commit();
        } catch (Exception ignored) {
        }
    }
}
