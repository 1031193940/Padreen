#include "vddpack.h"

namespace ph {

namespace {

constexpr int kMaxBits = 15;

struct Huffman {
    uint16_t count[kMaxBits + 1];     // number of codes of each length
    uint16_t symbol[288];             // the symbols, ordered by code
};

// A DEFLATE decoder written for clarity (it follows the structure of zlib's contrib/puff): bit by bit, with every bound checked.
class Inflater {
public:
    Inflater(const uint8_t* in, size_t inLen, size_t maxOut, Bytes& out) : in_(in), inLen_(inLen), maxOut_(maxOut), out_(out) {}

    bool run() {
        int last;
        do {
            last = bits(1);
            const int type = bits(2);
            if (fail_) return false;
            const bool ok = type == 0 ? stored() : type == 1 ? fixedBlock() : type == 2 ? dynamicBlock() : false;
            if (!ok) return false;
        } while (!last);
        return true;
    }

private:
    int bits(int need) {
        uint32_t val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= inLen_) {
                fail_ = true;
                return 0;
            }
            val |= (uint32_t)in_[pos_++] << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = val >> need;
        bitcnt_ -= need;
        return (int)(val & ((1u << need) - 1));
    }

    int decode(const Huffman& h) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            code |= bits(1);
            if (fail_) return 0;
            const int count = h.count[len];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        fail_ = true;       // no code of any length matched
        return 0;
    }

    // 0 = complete code, > 0 = incomplete, < 0 = over-subscribed
    static int construct(Huffman& h, const uint8_t* length, int n) {
        for (int len = 0; len <= kMaxBits; ++len) h.count[len] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;          // no codes at all (decode() will fail if one is needed)
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        uint16_t offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = (uint16_t)(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s)
            if (length[s] != 0) h.symbol[offs[length[s]]++] = (uint16_t)s;
        return left;
    }

    bool codes(const Huffman& lencode, const Huffman& distcode) {
        static const uint16_t kLens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const uint8_t kLext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const uint16_t kDists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const uint8_t kDext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym = decode(lencode);
            if (fail_) return false;
            if (sym < 256) {
                if (out_.size() >= maxOut_) return false;
                out_.push_back((uint8_t)sym);
            } else if (sym == 256) {
                return true;
            } else {
                sym -= 257;
                if (sym >= 29) return false;
                const int len = kLens[sym] + bits(kLext[sym]);
                const int ds = decode(distcode);
                if (fail_ || ds >= 30) return false;
                const size_t dist = (size_t)kDists[ds] + (size_t)bits(kDext[ds]);
                if (fail_ || dist > out_.size() || out_.size() + (size_t)len > maxOut_) return false;
                const size_t from = out_.size() - dist;
                for (int i = 0; i < len; ++i) {         // may overlap what is being written: byte by byte
                    const uint8_t b = out_[from + (size_t)i];
                    out_.push_back(b);
                }
            }
        }
    }

    bool stored() {
        bitbuf_ = 0;                 // the rest of the current byte is not used
        bitcnt_ = 0;
        if (pos_ + 4 > inLen_) return false;
        const unsigned len = in_[pos_] | ((unsigned)in_[pos_ + 1] << 8);
        const unsigned nlen = in_[pos_ + 2] | ((unsigned)in_[pos_ + 3] << 8);
        pos_ += 4;
        if (len != (~nlen & 0xffffu) || len > inLen_ - pos_ || out_.size() + len > maxOut_) return false;
        out_.insert(out_.end(), in_ + pos_, in_ + pos_ + len);
        pos_ += len;
        return true;
    }

    bool fixedBlock() {
        Huffman lc, dc;
        uint8_t lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        construct(lc, lengths, 288);
        for (s = 0; s < 30; ++s) lengths[s] = 5;
        construct(dc, lengths, 30);
        return codes(lc, dc);
    }

    bool dynamicBlock() {
        static const uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        uint8_t lengths[320];
        const int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (fail_ || nlen > 286 || ndist > 30) return false;
        int index = 0;
        for (; index < ncode; ++index) lengths[kOrder[index]] = (uint8_t)bits(3);
        for (; index < 19; ++index) lengths[kOrder[index]] = 0;
        if (fail_) return false;
        Huffman lc, dc;
        if (construct(lc, lengths, 19) != 0) return false;       // the code that describes the code lengths has to be complete
        index = 0;
        while (index < nlen + ndist) {
            int sym = decode(lc);
            if (fail_) return false;
            if (sym < 16) {
                lengths[index++] = (uint8_t)sym;
            } else {
                int len = 0;
                if (sym == 16) {
                    if (index == 0) return false;               // nothing to repeat
                    len = lengths[index - 1];
                    sym = 3 + bits(2);
                } else if (sym == 17) {
                    sym = 3 + bits(3);
                } else {
                    sym = 11 + bits(7);
                }
                if (fail_ || index + sym > nlen + ndist) return false;
                while (sym--) lengths[index++] = (uint8_t)len;
            }
        }
        if (lengths[256] == 0) return false;                     // a block without an end-of-block code
        int err = construct(lc, lengths, nlen);
        if (err && (err < 0 || nlen != lc.count[0] + lc.count[1])) return false;          // an incomplete code is only valid for a single code of length 1
        err = construct(dc, lengths + nlen, ndist);
        if (err && (err < 0 || ndist != dc.count[0] + dc.count[1])) return false;
        return codes(lc, dc);
    }

    const uint8_t* in_;
    size_t inLen_;
    size_t pos_ = 0;
    size_t maxOut_;
    Bytes& out_;
    uint32_t bitbuf_ = 0;
    int bitcnt_ = 0;
    bool fail_ = false;
};

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

}  // namespace

bool inflateRaw(const uint8_t* in, size_t inLen, size_t maxOut, Bytes& out) {
    out.clear();
    Inflater f(in, inLen, maxOut, out);
    if (f.run()) return true;
    out.clear();
    return false;
}

bool nsisFindFiles(const Bytes& inst, const std::vector<std::string>& wanted, std::vector<Bytes>& found) {
    found.assign(wanted.size(), Bytes());
    std::vector<bool> got(wanted.size(), false);
    // The first header sits on a 512-byte boundary:  flags(4)  signature(16: EF BE AD DE "NullsoftInst")  length_of_header(4)  length_of_all_data(4)
    static const uint8_t kSig[16] = {0xEF, 0xBE, 0xAD, 0xDE, 'N', 'u', 'l', 'l', 's', 'o', 'f', 't', 'I', 'n', 's', 't'};
    size_t fh = (size_t)-1;
    for (size_t p = 0; p + 28 <= inst.size(); p += 512)
        if (memcmp(&inst[p + 4], kSig, sizeof(kSig)) == 0) {
            fh = p;
            break;
        }
    if (fh == (size_t)-1) return false;
    const size_t total = rd32(&inst[fh + 24]);
    if (total < 28 || total > inst.size() - fh) return false;
    const size_t end = fh + total;                  // the data, then a 4-byte CRC
    constexpr size_t kMaxFile = 4u << 20;
    size_t pos = fh + 28;
    for (int blobs = 0; blobs < 4096 && pos + 4 <= end; ++blobs) {
        const uint32_t raw = rd32(&inst[pos]);
        const bool compressed = (raw & 0x80000000u) != 0;
        const size_t len = raw & 0x7fffffffu;
        if (len > end - (pos + 4)) break;           // what is left is the CRC, which does not look like a length
        const uint8_t* p = &inst[pos + 4];
        pos += 4 + len;
        Bytes data;
        if (compressed) {
            if (!inflateRaw(p, len, kMaxFile, data)) continue;      // not a deflate blob (or damaged): it is not one of the files we want
        } else {
            data.assign(p, p + len);
        }
        const Bytes h = sha256(data);
        const std::string hex = toHex(h.data(), h.size());
        for (size_t i = 0; i < wanted.size(); ++i)
            if (!got[i] && wanted[i] == hex) {
                found[i] = std::move(data);
                got[i] = true;
                break;
            }
    }
    for (bool g : got)
        if (!g) return false;
    return true;
}

}  // namespace ph
