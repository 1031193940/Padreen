import java.io.*;
import java.math.BigInteger;
import java.nio.file.*;
import java.security.*;
import java.security.spec.*;
import java.security.interfaces.ECPublicKey;
import javax.crypto.KeyAgreement;

public class EcdhTest {
    static byte[] pub65(ECPublicKey k) {
        byte[] out = new byte[65];
        out[0] = 4;
        byte[] x = k.getW().getAffineX().toByteArray(), y = k.getW().getAffineY().toByteArray();
        copyBE(x, out, 1);
        copyBE(y, out, 33);
        return out;
    }
    static void copyBE(byte[] v, byte[] out, int off) {
        int n = v.length > 32 ? 32 : v.length, src = v.length - n;
        System.arraycopy(v, src, out, off + 32 - n, n);
    }
    static String hex(byte[] b) { StringBuilder s = new StringBuilder(); for (byte x : b) s.append(String.format("%02x", x & 0xff)); return s.toString(); }
    static byte[] unhex(String h) { byte[] b = new byte[h.length() / 2]; for (int i = 0; i < b.length; i++) b[i] = (byte) Integer.parseInt(h.substring(2 * i, 2 * i + 2), 16); return b; }

    public static void main(String[] a) throws Exception {
        if (a[0].equals("gen")) {
            KeyPairGenerator g = KeyPairGenerator.getInstance("EC");
            g.initialize(new ECGenParameterSpec("secp256r1"));
            KeyPair kp = g.generateKeyPair();
            Files.write(Paths.get("java_pub.hex"), hex(pub65((ECPublicKey) kp.getPublic())).getBytes());
            Files.write(Paths.get("java_priv.bin"), kp.getPrivate().getEncoded());
            System.out.println("java pub written");
        } else {
            byte[] priv = Files.readAllBytes(Paths.get("java_priv.bin"));
            PrivateKey pk = KeyFactory.getInstance("EC").generatePrivate(new PKCS8EncodedKeySpec(priv));
            byte[] pc = unhex(new String(Files.readAllBytes(Paths.get("pc_pub.hex"))).trim());
            AlgorithmParameters ap = AlgorithmParameters.getInstance("EC");
            ap.init(new ECGenParameterSpec("secp256r1"));
            ECParameterSpec spec = ap.getParameterSpec(ECParameterSpec.class);
            BigInteger x = new BigInteger(1, java.util.Arrays.copyOfRange(pc, 1, 33)), y = new BigInteger(1, java.util.Arrays.copyOfRange(pc, 33, 65));
            PublicKey peer = KeyFactory.getInstance("EC").generatePublic(new ECPublicKeySpec(new ECPoint(x, y), spec));
            KeyAgreement ka = KeyAgreement.getInstance("ECDH");
            ka.init(pk);
            ka.doPhase(peer, true);
            String javaSecret = hex(ka.generateSecret());
            String pcSecret = new String(Files.readAllBytes(Paths.get("pc_secret.hex"))).trim();
            System.out.println("java secret " + javaSecret);
            System.out.println("pc   secret " + pcSecret);
            System.out.println(javaSecret.equals(pcSecret) ? "ECDH INTEROP OK" : "ECDH MISMATCH");
        }
    }
}
