/*
    abchash.cpp: MurmurHash3 x64-128 and SpookyHash V2 (see abchash.h)
*/

#include "abchash.h"
#include <cstring>

namespace abc {

static inline uint64_t rot64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

static inline uint64_t load64(const uint8_t *p) {
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline uint32_t load32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ------------------------------------------------------------------------- */
/*  MurmurHash3 x64-128                                                      */
/* ------------------------------------------------------------------------- */

static inline uint64_t fmix64(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdULL;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ULL;
    k ^= k >> 33;
    return k;
}

Digest murmur3(const void *key, size_t len) {
    const uint8_t *data = (const uint8_t *) key;
    const size_t nblocks = len / 16;
    const uint64_t c1 = 0x87c37b91114253d5ULL, c2 = 0x4cf5ad432745937fULL;
    uint64_t h1 = 0, h2 = 0;

    for (size_t i = 0; i < nblocks; ++i) {
        uint64_t k1 = load64(data + 16 * i), k2 = load64(data + 16 * i + 8);
        k1 *= c1; k1 = rot64(k1, 31); k1 *= c2; h1 ^= k1;
        h1 = rot64(h1, 27); h1 += h2; h1 = h1 * 5 + 0x52dce729;
        k2 *= c2; k2 = rot64(k2, 33); k2 *= c1; h2 ^= k2;
        h2 = rot64(h2, 31); h2 += h1; h2 = h2 * 5 + 0x38495ab5;
    }

    const uint8_t *tail = data + nblocks * 16;
    uint64_t k1 = 0, k2 = 0;
    switch (len & 15) {
        case 15: k2 ^= uint64_t(tail[14]) << 48; /* fallthrough */
        case 14: k2 ^= uint64_t(tail[13]) << 40; /* fallthrough */
        case 13: k2 ^= uint64_t(tail[12]) << 32; /* fallthrough */
        case 12: k2 ^= uint64_t(tail[11]) << 24; /* fallthrough */
        case 11: k2 ^= uint64_t(tail[10]) << 16; /* fallthrough */
        case 10: k2 ^= uint64_t(tail[9]) << 8;   /* fallthrough */
        case 9:  k2 ^= uint64_t(tail[8]);
                 k2 *= c2; k2 = rot64(k2, 33); k2 *= c1; h2 ^= k2;
                 /* fallthrough */
        case 8:  k1 ^= uint64_t(tail[7]) << 56; /* fallthrough */
        case 7:  k1 ^= uint64_t(tail[6]) << 48; /* fallthrough */
        case 6:  k1 ^= uint64_t(tail[5]) << 40; /* fallthrough */
        case 5:  k1 ^= uint64_t(tail[4]) << 32; /* fallthrough */
        case 4:  k1 ^= uint64_t(tail[3]) << 24; /* fallthrough */
        case 3:  k1 ^= uint64_t(tail[2]) << 16; /* fallthrough */
        case 2:  k1 ^= uint64_t(tail[1]) << 8;  /* fallthrough */
        case 1:  k1 ^= uint64_t(tail[0]);
                 k1 *= c1; k1 = rot64(k1, 31); k1 *= c2; h1 ^= k1;
                 break;
        default: break;
    }

    h1 ^= len; h2 ^= len;
    h1 += h2; h2 += h1;
    h1 = fmix64(h1); h2 = fmix64(h2);
    h1 += h2; h2 += h1;

    Digest d;
    d.words[0] = h1;
    d.words[1] = h2;
    return d;
}

/* ------------------------------------------------------------------------- */
/*  SpookyHash V2                                                            */
/* ------------------------------------------------------------------------- */

static const uint64_t SC_CONST = 0xdeadbeefdeadbeefULL;

static inline void mix(const uint64_t *data,
                       uint64_t &s0, uint64_t &s1, uint64_t &s2, uint64_t &s3,
                       uint64_t &s4, uint64_t &s5, uint64_t &s6, uint64_t &s7,
                       uint64_t &s8, uint64_t &s9, uint64_t &s10, uint64_t &s11) {
    s0 += data[0];   s2 ^= s10;  s11 ^= s0;  s0 = rot64(s0, 11);   s11 += s1;
    s1 += data[1];   s3 ^= s11;  s0 ^= s1;   s1 = rot64(s1, 32);   s0 += s2;
    s2 += data[2];   s4 ^= s0;   s1 ^= s2;   s2 = rot64(s2, 43);   s1 += s3;
    s3 += data[3];   s5 ^= s1;   s2 ^= s3;   s3 = rot64(s3, 31);   s2 += s4;
    s4 += data[4];   s6 ^= s2;   s3 ^= s4;   s4 = rot64(s4, 17);   s3 += s5;
    s5 += data[5];   s7 ^= s3;   s4 ^= s5;   s5 = rot64(s5, 28);   s4 += s6;
    s6 += data[6];   s8 ^= s4;   s5 ^= s6;   s6 = rot64(s6, 39);   s5 += s7;
    s7 += data[7];   s9 ^= s5;   s6 ^= s7;   s7 = rot64(s7, 57);   s6 += s8;
    s8 += data[8];   s10 ^= s6;  s7 ^= s8;   s8 = rot64(s8, 55);   s7 += s9;
    s9 += data[9];   s11 ^= s7;  s8 ^= s9;   s9 = rot64(s9, 54);   s8 += s10;
    s10 += data[10]; s0 ^= s8;   s9 ^= s10;  s10 = rot64(s10, 22); s9 += s11;
    s11 += data[11]; s1 ^= s9;   s10 ^= s11; s11 = rot64(s11, 46); s10 += s0;
}

static inline void end_partial(uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3,
                               uint64_t &h4, uint64_t &h5, uint64_t &h6, uint64_t &h7,
                               uint64_t &h8, uint64_t &h9, uint64_t &h10, uint64_t &h11) {
    h11 += h1;  h2 ^= h11;  h1 = rot64(h1, 44);
    h0 += h2;   h3 ^= h0;   h2 = rot64(h2, 15);
    h1 += h3;   h4 ^= h1;   h3 = rot64(h3, 34);
    h2 += h4;   h5 ^= h2;   h4 = rot64(h4, 21);
    h3 += h5;   h6 ^= h3;   h5 = rot64(h5, 38);
    h4 += h6;   h7 ^= h4;   h6 = rot64(h6, 33);
    h5 += h7;   h8 ^= h5;   h7 = rot64(h7, 10);
    h6 += h8;   h9 ^= h6;   h8 = rot64(h8, 13);
    h7 += h9;   h10 ^= h7;  h9 = rot64(h9, 38);
    h8 += h10;  h11 ^= h8;  h10 = rot64(h10, 53);
    h9 += h11;  h0 ^= h9;   h11 = rot64(h11, 42);
    h10 += h0;  h1 ^= h10;  h0 = rot64(h0, 54);
}

static inline void end(const uint64_t *data,
                       uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3,
                       uint64_t &h4, uint64_t &h5, uint64_t &h6, uint64_t &h7,
                       uint64_t &h8, uint64_t &h9, uint64_t &h10, uint64_t &h11) {
    h0 += data[0]; h1 += data[1]; h2 += data[2];   h3 += data[3];
    h4 += data[4]; h5 += data[5]; h6 += data[6];   h7 += data[7];
    h8 += data[8]; h9 += data[9]; h10 += data[10]; h11 += data[11];
    end_partial(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
    end_partial(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
    end_partial(h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
}

static inline void short_mix(uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3) {
    h2 = rot64(h2, 50); h2 += h3; h0 ^= h2;
    h3 = rot64(h3, 52); h3 += h0; h1 ^= h3;
    h0 = rot64(h0, 30); h0 += h1; h2 ^= h0;
    h1 = rot64(h1, 41); h1 += h2; h3 ^= h1;
    h2 = rot64(h2, 54); h2 += h3; h0 ^= h2;
    h3 = rot64(h3, 48); h3 += h0; h1 ^= h3;
    h0 = rot64(h0, 38); h0 += h1; h2 ^= h0;
    h1 = rot64(h1, 37); h1 += h2; h3 ^= h1;
    h2 = rot64(h2, 62); h2 += h3; h0 ^= h2;
    h3 = rot64(h3, 34); h3 += h0; h1 ^= h3;
    h0 = rot64(h0, 5);  h0 += h1; h2 ^= h0;
    h1 = rot64(h1, 36); h1 += h2; h3 ^= h1;
}

void SpookyHash::short_end(uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3) {
    h3 ^= h2; h2 = rot64(h2, 15); h3 += h2;
    h0 ^= h3; h3 = rot64(h3, 52); h0 += h3;
    h1 ^= h0; h0 = rot64(h0, 26); h1 += h0;
    h2 ^= h1; h1 = rot64(h1, 51); h2 += h1;
    h3 ^= h2; h2 = rot64(h2, 28); h3 += h2;
    h0 ^= h3; h3 = rot64(h3, 9);  h0 += h3;
    h1 ^= h0; h0 = rot64(h0, 47); h1 += h0;
    h2 ^= h1; h1 = rot64(h1, 54); h2 += h1;
    h3 ^= h2; h2 = rot64(h2, 32); h3 += h2;
    h0 ^= h3; h3 = rot64(h3, 25); h0 += h3;
    h1 ^= h0; h0 = rot64(h0, 63); h1 += h0;
}

void SpookyHash::short_hash(const void *message, size_t length, uint64_t *hash1, uint64_t *hash2) {
    const uint8_t *p = (const uint8_t *) message;
    size_t remainder = length % 32;
    uint64_t a = *hash1, b = *hash2, c = SC_CONST, d = SC_CONST;

    if (length > 15) {
        const uint8_t *endp = p + (length / 32) * 32;
        for (; p < endp; p += 32) {
            c += load64(p);
            d += load64(p + 8);
            short_mix(a, b, c, d);
            a += load64(p + 16);
            b += load64(p + 24);
        }
        if (remainder >= 16) {
            c += load64(p);
            d += load64(p + 8);
            short_mix(a, b, c, d);
            p += 16;
            remainder -= 16;
        }
    }

    d += ((uint64_t) length) << 56;
    switch (remainder) {
        case 15: d += ((uint64_t) p[14]) << 48; /* fallthrough */
        case 14: d += ((uint64_t) p[13]) << 40; /* fallthrough */
        case 13: d += ((uint64_t) p[12]) << 32; /* fallthrough */
        case 12: d += load32(p + 8); c += load64(p); break;
        case 11: d += ((uint64_t) p[10]) << 16; /* fallthrough */
        case 10: d += ((uint64_t) p[9]) << 8;   /* fallthrough */
        case 9:  d += (uint64_t) p[8];          /* fallthrough */
        case 8:  c += load64(p); break;
        case 7:  c += ((uint64_t) p[6]) << 48;  /* fallthrough */
        case 6:  c += ((uint64_t) p[5]) << 40;  /* fallthrough */
        case 5:  c += ((uint64_t) p[4]) << 32;  /* fallthrough */
        case 4:  c += load32(p); break;
        case 3:  c += ((uint64_t) p[2]) << 16;  /* fallthrough */
        case 2:  c += ((uint64_t) p[1]) << 8;   /* fallthrough */
        case 1:  c += (uint64_t) p[0]; break;
        case 0:  c += SC_CONST; d += SC_CONST; break;
    }
    short_end(a, b, c, d);
    *hash1 = a;
    *hash2 = b;
}

void SpookyHash::init(uint64_t seed1, uint64_t seed2) {
    mLength = 0;
    mRemainder = 0;
    mState[0] = seed1;
    mState[1] = seed2;
}

void SpookyHash::update(const void *message, size_t length) {
    uint64_t h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11;
    const size_t newLength = length + mRemainder;
    const uint8_t *p = (const uint8_t *) message;

    if (newLength < BufSize) {
        memcpy(&((uint8_t *) mData)[mRemainder], message, length);
        mLength = length + mLength;
        mRemainder = (uint8_t) newLength;
        return;
    }

    if (mLength < BufSize) {
        h0 = h3 = h6 = h9 = mState[0];
        h1 = h4 = h7 = h10 = mState[1];
        h2 = h5 = h8 = h11 = SC_CONST;
    } else {
        h0 = mState[0]; h1 = mState[1]; h2 = mState[2];   h3 = mState[3];
        h4 = mState[4]; h5 = mState[5]; h6 = mState[6];   h7 = mState[7];
        h8 = mState[8]; h9 = mState[9]; h10 = mState[10]; h11 = mState[11];
    }
    mLength = length + mLength;

    if (mRemainder) {
        const uint8_t prefix = (uint8_t) (BufSize - mRemainder);
        memcpy(&((uint8_t *) mData)[mRemainder], message, prefix);
        mix(mData, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
        mix(&mData[NumVars], h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
        p += prefix;
        length -= prefix;
    }

    const uint8_t *endp = p + (length / BlockSize) * BlockSize;
    const uint8_t remainder = (uint8_t) (length - (endp - p));
    uint64_t block[NumVars];
    for (; p < endp; p += BlockSize) {
        memcpy(block, p, BlockSize);
        mix(block, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
    }

    mRemainder = remainder;
    memcpy(mData, endp, remainder);
    mState[0] = h0; mState[1] = h1; mState[2] = h2;   mState[3] = h3;
    mState[4] = h4; mState[5] = h5; mState[6] = h6;   mState[7] = h7;
    mState[8] = h8; mState[9] = h9; mState[10] = h10; mState[11] = h11;
}

void SpookyHash::final(uint64_t *hash1, uint64_t *hash2) {
    if (mLength < BufSize) {
        *hash1 = mState[0];
        *hash2 = mState[1];
        short_hash(mData, mLength, hash1, hash2);
        return;
    }

    uint64_t *data = mData;
    uint8_t remainder = mRemainder;
    uint64_t h0 = mState[0], h1 = mState[1], h2 = mState[2],   h3 = mState[3];
    uint64_t h4 = mState[4], h5 = mState[5], h6 = mState[6],   h7 = mState[7];
    uint64_t h8 = mState[8], h9 = mState[9], h10 = mState[10], h11 = mState[11];

    if (remainder >= BlockSize) {
        mix(data, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
        data += NumVars;
        remainder = (uint8_t) (remainder - BlockSize);
    }

    /* Pads the buffer in place, exactly like the reference implementation */
    memset(&((uint8_t *) data)[remainder], 0, BlockSize - remainder);
    ((uint8_t *) data)[BlockSize - 1] = remainder;
    end(data, h0, h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11);
    *hash1 = h0;
    *hash2 = h1;
}

} // namespace abc
