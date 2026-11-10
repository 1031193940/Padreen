package com.padreen.app;

/** Minimal LZ4 block decoder (the PC compresses gray8 settle frames with LZ4_compress_default). */
final class Lz4 {
    private Lz4() {
    }

    /**
     * @return number of bytes written to dst, or -1 if the input is malformed / does not fit.
     */
    static int decompress(byte[] src, int srcOff, int srcLen, byte[] dst, int dstOff, int dstCap) {
        int sp = srcOff;
        final int sEnd = srcOff + srcLen;
        int dp = dstOff;
        final int dEnd = dstOff + dstCap;
        while (sp < sEnd) {
            int token = src[sp++] & 0xff;
            int lit = token >>> 4;
            if (lit == 15) {
                int b;
                do {
                    if (sp >= sEnd) return -1;
                    b = src[sp++] & 0xff;
                    lit += b;
                } while (b == 255);
            }
            if (lit > 0) {
                if (sp + lit > sEnd || dp + lit > dEnd) return -1;
                System.arraycopy(src, sp, dst, dp, lit);
                sp += lit;
                dp += lit;
            }
            if (sp >= sEnd) break;                       // the last sequence carries literals only
            if (sp + 2 > sEnd) return -1;
            int offset = (src[sp] & 0xff) | ((src[sp + 1] & 0xff) << 8);
            sp += 2;
            if (offset == 0) return -1;
            int mlen = token & 15;
            if (mlen == 15) {
                int b;
                do {
                    if (sp >= sEnd) return -1;
                    b = src[sp++] & 0xff;
                    mlen += b;
                } while (b == 255);
            }
            mlen += 4;
            int mp = dp - offset;
            if (mp < dstOff || dp + mlen > dEnd) return -1;
            if (offset >= mlen) {
                System.arraycopy(dst, mp, dst, dp, mlen);
                dp += mlen;
            } else {                                      // overlapping run: byte by byte
                for (int i = 0; i < mlen; i++) dst[dp++] = dst[mp++];
            }
        }
        return dp - dstOff;
    }
}
