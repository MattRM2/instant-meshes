/*
    abchash.h: The two hash functions of the Alembic file format.

    - MurmurHash3 (x64, 128 bit, seed 0) keys every sample: the 16 bytes
      stored in front of the values. Readers use them as cache keys, so they
      must match the data exactly.
    - SpookyHash V2 chains the header and sample hashes of properties and
      objects into the 32 bytes stored after each object's child headers.

    Both algorithms are public domain (MurmurHash3 by Austin Appleby,
    SpookyHash by Bob Jenkins); this is an independent transcription that
    reproduces the byte-exact results of the Alembic library on
    little-endian hosts.
*/

#pragma once

#include <cstdint>
#include <cstddef>

namespace abc {

struct Digest {
    uint64_t words[2] = { 0, 0 };
    bool operator==(const Digest &d) const {
        return words[0] == d.words[0] && words[1] == d.words[1];
    }
    bool operator!=(const Digest &d) const { return !(*this == d); }
};

/// MurmurHash3_x64_128 with seed 0
Digest murmur3(const void *data, size_t size);

class SpookyHash {
public:
    void init(uint64_t seed1 = 0, uint64_t seed2 = 0);
    void update(const void *data, size_t size);
    /// Like the reference implementation, final() may be followed by more
    /// update() calls (Alembic relies on this when hashing objects)
    void final(uint64_t *hash1, uint64_t *hash2);
    Digest final() { Digest d; final(&d.words[0], &d.words[1]); return d; }

    static void short_end(uint64_t &h0, uint64_t &h1, uint64_t &h2, uint64_t &h3);

private:
    static const size_t NumVars = 12;
    static const size_t BlockSize = NumVars * 8;
    static const size_t BufSize = 2 * BlockSize;

    static void short_hash(const void *message, size_t length, uint64_t *hash1, uint64_t *hash2);

    uint64_t mData[2 * NumVars];
    uint64_t mState[NumVars];
    size_t mLength = 0;
    uint8_t mRemainder = 0;
};

} // namespace abc
