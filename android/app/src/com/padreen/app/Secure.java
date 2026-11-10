package com.padreen.app;

import android.content.Context;
import android.content.SharedPreferences;
import android.provider.Settings;

import java.security.MessageDigest;
import java.security.SecureRandom;

import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

/** This tablet's identity and the small crypto helpers of the PD2 handshake (see protocol/PROTOCOL.md). */
final class Secure {
    private static final String PREFS = "pd";

    private Secure() {
    }

    /** Random id of this installation (the "padId" the PC pairs with). */
    static byte[] deviceId(Context c) {
        SharedPreferences p = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        String h = p.getString("dev", null);
        if (h == null || h.length() != 32) {
            byte[] id = new byte[16];
            new SecureRandom().nextBytes(id);
            h = toHex(id);
            p.edit().putString("dev", h).commit();
        }
        return fromHex(h);
    }

    /** What the user would call this tablet: the name set in Settings, else the model. */
    static String deviceName(Context c) {
        String n = null;
        try {
            n = Settings.Global.getString(c.getContentResolver(), "device_name");
        } catch (Throwable ignored) {
        }
        if (n == null || n.trim().isEmpty()) n = android.os.Build.MODEL;
        if (n == null || n.trim().isEmpty()) n = "Tablet";
        n = n.trim();
        byte[] b = n.getBytes(java.nio.charset.Charset.forName("UTF-8"));
        while (b.length > 40) {                           // the wire format has a one-byte length and the PC shows at most 40 bytes
            n = n.substring(0, n.length() - 1);
            b = n.getBytes(java.nio.charset.Charset.forName("UTF-8"));
        }
        return n;
    }

    /** First 4 bytes of SHA-256(key): lets the PC check whether the tablet holds ITS key without revealing it. */
    static byte[] keyId(byte[] key) {
        byte[] d = sha256(key);
        byte[] id = new byte[4];
        System.arraycopy(d, 0, id, 0, 4);
        return id;
    }

    static byte[] sha256(byte[]... parts) {
        try {
            MessageDigest md = MessageDigest.getInstance("SHA-256");
            for (byte[] p : parts) md.update(p);
            return md.digest();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] random(int n) {
        byte[] b = new byte[n];
        new SecureRandom().nextBytes(b);
        return b;
    }

    static byte[] hmac(byte[] key, byte[]... parts) {
        try {
            Mac m = Mac.getInstance("HmacSHA256");
            m.init(new SecretKeySpec(key, "HmacSHA256"));
            for (byte[] p : parts) m.update(p);
            return m.doFinal();
        } catch (Exception e) {
            throw new IllegalStateException(e);
        }
    }

    static byte[] cat(byte[]... parts) {
        int n = 0;
        for (byte[] p : parts) n += p.length;
        byte[] o = new byte[n];
        int off = 0;
        for (byte[] p : parts) {
            System.arraycopy(p, 0, o, off, p.length);
            off += p.length;
        }
        return o;
    }

    static boolean equal(byte[] a, byte[] b) {
        return MessageDigest.isEqual(a, b);       // constant time
    }

    static byte[] ascii(String s) {
        return s.getBytes(java.nio.charset.Charset.forName("US-ASCII"));
    }

    static String toHex(byte[] b) {
        StringBuilder sb = new StringBuilder();
        for (byte x : b) sb.append(String.format("%02x", x & 0xff));
        return sb.toString();
    }

    static byte[] fromHex(String h) {
        byte[] b = new byte[h.length() / 2];
        for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16);
        return b;
    }
}
