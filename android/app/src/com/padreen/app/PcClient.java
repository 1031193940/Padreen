package com.padreen.app;

import android.content.Context;

import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.math.BigInteger;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PublicKey;
import java.security.interfaces.ECPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPoint;
import java.security.spec.ECPublicKeySpec;
import java.util.Arrays;

import javax.crypto.KeyAgreement;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;

/**
 * The tablet side of the PD2 handshake (protocol/PROTOCOL.md): connect to a paired PC (mutual HMAC proof, then AES-GCM records), or pair
 * with a new PC using the 9-digit code from its pairing window.  Blocking - call from a background thread.
 */
final class PcClient {
    static final int T_HELLO = 0x80, T_CHALLENGE = 0x81, T_AUTH = 0x82, T_AUTH_OK = 0x83, T_FAIL = 0x84, T_PAIR_CH = 0x85, T_PAIR_PROOF = 0x86,
            T_PAIR_OK = 0x87;
    static final int FAIL_UNPAIRED = 1, FAIL_AUTH = 2, FAIL_BUSY = 3, FAIL_NOT_PAIRING = 4, FAIL_CODE = 5, FAIL_DISABLED = 6, FAIL_PROTO = 7;

    /** The PC said no (or something else went wrong with a known cause). */
    static final class Failure extends IOException {
        final int reason;

        Failure(int reason, String msg) {
            super(msg);
            this.reason = reason;
        }
    }

    /** An authenticated, encrypted connection ready for the display session. */
    static final class Result {
        Socket socket;
        Chan chan;
        PcStore.Pc pc;
    }

    private PcClient() {
    }

    private static final class Wire {
        final Socket socket;
        final InputStream in;
        final OutputStream out;
        final Chan.Plain plain;
        final byte[] hdr = new byte[8];
        final byte[] body = new byte[1024];

        Wire(String ip, int port) throws IOException {
            socket = new Socket();
            socket.connect(new InetSocketAddress(ip, port), 4000);
            socket.setTcpNoDelay(true);
            socket.setSoTimeout(8000);
            in = socket.getInputStream();
            out = socket.getOutputStream();
            plain = new Chan.Plain(in, out);
        }

        /** Reads the next handshake frame; a PD_FAIL becomes a {@link Failure}. Returns the payload. */
        byte[] expect(int type) throws IOException {
            int len = plain.readFrame(hdr, body);
            int t = hdr[0] & 0xff;
            if (t == T_FAIL) {
                int r = len > 0 ? body[0] & 0xff : 0;
                throw new Failure(r, "PC refused (" + r + ")");
            }
            if (t != type) throw new Failure(FAIL_PROTO, "unexpected frame 0x" + Integer.toHexString(t));
            return Arrays.copyOf(body, len);
        }

        void close() {
            try {
                socket.close();
            } catch (IOException ignored) {
            }
        }
    }

    private static byte[] helloPayload(Context ctx, int mode) {
        byte[] name = Secure.deviceName(ctx).getBytes(java.nio.charset.Charset.forName("UTF-8"));
        byte[] h = new byte[19 + name.length];
        h[0] = 2;
        h[1] = (byte) mode;
        System.arraycopy(Secure.deviceId(ctx), 0, h, 2, 16);
        h[18] = (byte) name.length;
        System.arraycopy(name, 0, h, 19, name.length);
        return h;
    }

    // ------------------------------------------------------------------------------------------------------------ connect (paired)
    /** Largest data-plane payload (a full 1872x1404 gray frame plus headers) - sizes the record buffers. */
    static final int MAX_PAYLOAD = 1872 * 1404 + 4096;

    static Result connect(Context ctx, PcStore.Pc pc, String ip, int port) throws IOException {
        Wire w = new Wire(ip, port);
        boolean ok = false;
        try {
            byte[] padId = Secure.deviceId(ctx);
            byte[] hello = helloPayload(ctx, 0);
            w.plain.write(T_HELLO, hello, hello.length);
            byte[] ch = w.expect(T_CHALLENGE);
            if (ch.length < 33) throw new Failure(FAIL_PROTO, "short challenge");
            byte[] pcId = Arrays.copyOfRange(ch, 0, 16), nonceS = Arrays.copyOfRange(ch, 16, 32);
            if (!Arrays.equals(pcId, pc.id)) throw new Failure(FAIL_AUTH, "this is a different computer");
            byte[] nonceC = Secure.random(16);
            byte[] macC = Secure.hmac(pc.key, Secure.ascii("PD2-auth-c"), pcId, padId, nonceS, nonceC);
            byte[] auth = Secure.cat(nonceC, macC);
            w.plain.write(T_AUTH, auth, auth.length);
            byte[] macS = w.expect(T_AUTH_OK);
            byte[] expect = Secure.hmac(pc.key, Secure.ascii("PD2-auth-s"), padId, pcId, nonceC, nonceS);
            if (!Secure.equal(expect, macS)) throw new Failure(FAIL_AUTH, "the computer failed to prove the pairing key");
            byte[] prk = Secure.hmac(Secure.cat(nonceS, nonceC), pc.key);
            byte[] c2s = Arrays.copyOf(Secure.hmac(prk, Secure.ascii("pd2 c2s"), new byte[]{1}), 16);
            byte[] s2c = Arrays.copyOf(Secure.hmac(prk, Secure.ascii("pd2 s2c"), new byte[]{1}), 16);
            w.socket.setSoTimeout(10000);       // the PC pings every 50 ms: 10 s of silence = the PC (or Wi-Fi) is gone, do not hang on a dead display
            Result r = new Result();
            r.socket = w.socket;
            r.chan = new Chan.Gcm(w.in, w.out, s2c, c2s, MAX_PAYLOAD);     // we receive what the PC encrypts with s2c, send with c2s
            r.pc = pc;
            ok = true;
            return r;
        } finally {
            if (!ok) w.close();
        }
    }

    // ------------------------------------------------------------------------------------------------------------ pair (new PC)
    static PcStore.Pc pair(Context ctx, String ip, int port, String code) throws IOException {
        Wire w = new Wire(ip, port);
        try {
            byte[] padId = Secure.deviceId(ctx);
            byte[] hello = helloPayload(ctx, 1);
            w.plain.write(T_HELLO, hello, hello.length);
            byte[] ch = w.expect(T_PAIR_CH);
            if (ch.length < 16 + 1 + 65 + 16 + 1) throw new Failure(FAIL_PROTO, "short pairing challenge");
            byte[] pcId = Arrays.copyOfRange(ch, 0, 16);
            int nl = ch[16] & 0xff;
            if (17 + nl + 65 + 16 + 1 > ch.length) throw new Failure(FAIL_PROTO, "bad pairing challenge");
            String pcName = new String(ch, 17, nl, java.nio.charset.Charset.forName("UTF-8"));
            byte[] pubS = Arrays.copyOfRange(ch, 17 + nl, 17 + nl + 65);
            byte[] salt = Arrays.copyOfRange(ch, 17 + nl + 65, 17 + nl + 65 + 16);

            KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
            g.initialize(new ECGenParameterSpec("secp256r1"));
            KeyPair kp = g.generateKeyPair();
            byte[] pubC = encodePublic((ECPublicKey) kp.getPublic());
            byte[] kpw = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
                    .generateSecret(new PBEKeySpec(code.toCharArray(), salt, 100000, 256)).getEncoded();
            byte[] transcript = Secure.sha256(Secure.ascii("PD2-pair"), hello, ch, pubC);
            byte[] proofC = Secure.hmac(kpw, Secure.ascii("PD2-pair-c"), transcript);
            byte[] pl = Secure.cat(pubC, proofC);
            w.plain.write(T_PAIR_PROOF, pl, pl.length);
            byte[] proofS = w.expect(T_PAIR_OK);
            if (!Secure.equal(Secure.hmac(kpw, Secure.ascii("PD2-pair-s"), transcript), proofS)) throw new Failure(FAIL_AUTH, "the computer failed the code check");
            KeyAgreement ka = KeyAgreement.getInstance("ECDH");
            ka.init(kp.getPrivate());
            ka.doPhase(decodePublic(pubS), true);
            byte[] z = ka.generateSecret();
            PcStore.Pc pc = new PcStore.Pc();
            pc.id = pcId;
            pc.name = pcName;
            pc.key = Secure.hmac(z, Secure.ascii("PD2-key"), transcript);
            pc.ip = ip;
            pc.port = port;
            pc.lastUsed = System.currentTimeMillis();
            return pc;
        } catch (IOException e) {
            throw e;
        } catch (Exception e) {
            throw new IOException("pairing crypto failed: " + e, e);
        } finally {
            w.close();
        }
    }

    // ------------------------------------------------------------------------------------------------------------ EC helpers
    private static byte[] encodePublic(ECPublicKey k) {
        byte[] out = new byte[65];
        out[0] = 4;
        copyBE(k.getW().getAffineX().toByteArray(), out, 1);
        copyBE(k.getW().getAffineY().toByteArray(), out, 33);
        return out;
    }

    private static void copyBE(byte[] v, byte[] out, int off) {
        int n = v.length > 32 ? 32 : v.length, src = v.length - n;
        System.arraycopy(v, src, out, off + 32 - n, n);
    }

    private static PublicKey decodePublic(byte[] p) throws Exception {
        if (p.length != 65 || p[0] != 4) throw new IOException("bad public key");
        AlgorithmParameters ap = AlgorithmParameters.getInstance("EC");
        ap.init(new ECGenParameterSpec("secp256r1"));
        ECParameterSpec spec = ap.getParameterSpec(ECParameterSpec.class);
        BigInteger x = new BigInteger(1, Arrays.copyOfRange(p, 1, 33)), y = new BigInteger(1, Arrays.copyOfRange(p, 33, 65));
        return KeyFactory.getInstance("EC").generatePublic(new ECPublicKeySpec(new ECPoint(x, y), spec));
    }
}
