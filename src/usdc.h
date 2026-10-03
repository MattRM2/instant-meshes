/*
    usdc.h: the Crate (.usdc) file reader behind usd::Layer (see usdc.cpp)
*/

#pragma once

#include "usd.h"
#include <fstream>

namespace usd {

/// LZ4 block decompression (exposed for the tests)
size_t lz4_block(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize);
/// Pixar's chunked LZ4 framing (TfFastCompression)
std::vector<uint8_t> fast_decompress(const uint8_t *src, size_t srcSize, size_t outSize);

class CrateFile {
public:
    struct Field { uint32_t token = 0; uint64_t rep = 0; };
    struct Spec { uint32_t path = 0, fieldSet = 0, type = 0; };
    enum SpecType {
        SpecUnknown = 0, SpecAttribute, SpecConnection, SpecExpression, SpecMapper, SpecMapperArg,
        SpecPrim, SpecPseudoRoot, SpecRelationship, SpecRelationshipTarget, SpecVariant, SpecVariantSet
    };

    /// 'start' / 'size': the file inside a .usdz package (size 0: to the end)
    CrateFile(const std::string &filename, uint64_t start = 0, uint64_t size = 0);

    /// Reads the bootstrap, the table of contents and every structural section
    void parse();

    /// Decodes a value rep (reads the file for non-inlined values)
    Value value(uint64_t rep);

    const std::string &token(uint64_t index) const;
    const std::string &string(uint64_t index) const;
    const std::string &path(uint64_t index) const;
    uint32_t version() const { return mVersion; }

    const std::vector<Field> &fields() const { return mFields; }
    const std::vector<uint32_t> &field_sets() const { return mFieldSets; }
    const std::vector<Spec> &specs() const { return mSpecs; }

    [[noreturn]] void fail(const std::string &msg) const;

private:
    void read(uint64_t pos, uint64_t size, void *out);
    std::vector<uint8_t> bytes(uint64_t &pos, uint64_t size);
    template <typename T> T get(uint64_t &pos) {
        T v;
        read(pos, sizeof(T), &v);
        pos += sizeof(T);
        return v;
    }
    template <typename T> std::vector<T> vector_of(uint64_t &pos, uint64_t count) {
        if (count > mSize / sizeof(T) + 1)
            fail("array larger than the file");
        std::vector<T> v((size_t) count);
        if (count > 0)
            read(pos, count * sizeof(T), v.data());
        pos += count * sizeof(T);
        return v;
    }
    template <typename Int> std::vector<Int> compressed_ints(uint64_t &pos, uint64_t count);
    /* Element count of an array: before 0.5.0 a rank (always 1) and a
       32-bit size, before 0.7.0 a 32-bit size, then 64 bits */
    uint64_t array_size(uint64_t &pos) {
        if (mVersion < 0x000500)
            get<uint32_t>(pos);
        return mVersion < 0x000700 ? get<uint32_t>(pos) : get<uint64_t>(pos);
    }
    void build_paths(const std::vector<uint32_t> &pathIndexes, const std::vector<int32_t> &elementTokens,
                     const std::vector<int32_t> &jumps);
    void old_paths(uint64_t &pos);

    std::string mFilename;
    std::ifstream mStream;
    uint64_t mStart = 0, mSize = 0;
    uint32_t mVersion = 0;    ///< 0x00MMmmpp
    std::vector<std::string> mTokens;
    std::vector<uint32_t> mStrings;
    std::vector<Field> mFields;
    std::vector<uint32_t> mFieldSets;
    std::vector<std::string> mPaths;
    std::vector<Spec> mSpecs;
};

} // namespace usd
