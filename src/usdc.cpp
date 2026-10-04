/*
    usdc.cpp: .usdc (Crate) layers, read without any USD library

    File layout (Pixar's crateFile.cpp, versions 0.0.1 to 0.12):
      bootstrap   "PXR-USDC", version (major, minor, patch), table of contents offset
      sections    TOKENS (null-separated strings), STRINGS (token indices),
                  FIELDS (token index + value rep), FIELDSETS (field indices,
                  ~0 separated), PATHS (a tree: element token + jumps), SPECS
                  (path, field set, spec type)
    From 0.4.0 the sections are compressed: LZ4 blocks in Pixar's chunked
    framing, integers delta coded with 2-bit size codes. A value rep is 64
    bits: array flag (63), inlined (62), compressed (61), type (48-55),
    payload (0-47: the value itself when inlined, else its file offset).
*/

#include "usd.h"
#include "usdc.h"
#include <fstream>
#include <cstring>

namespace usd {

/* ------------------------------------------------------------------------- */
/*  Decompression                                                            */
/* ------------------------------------------------------------------------- */

/* One LZ4 block (no frame); returns the decompressed size */
size_t lz4_block(const uint8_t *src, size_t srcSize, uint8_t *dst, size_t dstSize) {
    const uint8_t *ip = src, *iend = src + srcSize;
    uint8_t *op = dst, *oend = dst + dstSize;
    auto fail = []() -> size_t { throw std::runtime_error("USD: corrupted compressed data (LZ4)!"); };
    while (ip < iend) {
        const uint8_t token = *ip++;
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t b;
            do {
                if (ip >= iend)
                    return fail();
                b = *ip++;
                literals += b;
            } while (b == 255);
        }
        if (literals > (size_t) (iend - ip) || literals > (size_t) (oend - op))
            return fail();
        memcpy(op, ip, literals);
        ip += literals;
        op += literals;
        if (ip >= iend)
            break;    /* the last sequence has literals only */
        if (iend - ip < 2)
            return fail();
        const size_t offset = (size_t) ip[0] | ((size_t) ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > (size_t) (op - dst))
            return fail();
        size_t match = (token & 15) + 4;
        if ((token & 15) == 15) {
            uint8_t b;
            do {
                if (ip >= iend)
                    return fail();
                b = *ip++;
                match += b;
            } while (b == 255);
        }
        if (match > (size_t) (oend - op))
            return fail();
        const uint8_t *from = op - offset;
        for (size_t k = 0; k < match; ++k)   /* may overlap */
            op[k] = from[k];
        op += match;
    }
    return (size_t) (op - dst);
}

/* Pixar's TfFastCompression framing: a chunk count (0: one LZ4 block),
   then (int32 size, block) per chunk */
std::vector<uint8_t> fast_decompress(const uint8_t *src, size_t srcSize, size_t outSize) {
    std::vector<uint8_t> out(outSize);
    if (srcSize == 0)
        throw std::runtime_error("USD: empty compressed data!");
    const uint8_t chunks = src[0];
    size_t done = 0;
    if (chunks == 0) {
        done = lz4_block(src + 1, srcSize - 1, out.data(), outSize);
    } else {
        size_t pos = 1;
        for (uint8_t c = 0; c < chunks; ++c) {
            if (srcSize - pos < 4)
                throw std::runtime_error("USD: corrupted compressed data (chunks)!");
            int32_t size;
            memcpy(&size, src + pos, 4);
            pos += 4;
            if (size < 0 || (size_t) size > srcSize - pos)
                throw std::runtime_error("USD: corrupted compressed data (chunks)!");
            done += lz4_block(src + pos, (size_t) size, out.data() + done, outSize - done);
            pos += (size_t) size;
        }
    }
    out.resize(done);   /* 'outSize' is an upper bound (the working space of the integer coding) */
    return out;
}

/* Usd_IntegerCompression: a common delta, 2-bit codes (4 per byte), then
   the other deltas as small / medium / large integers */
template <typename Int>
std::vector<Int> decode_ints(const std::vector<uint8_t> &data, size_t count) {
    typedef typename std::conditional<sizeof(Int) == 4, int32_t, int64_t>::type SInt;
    typedef typename std::conditional<sizeof(Int) == 4, int8_t, int16_t>::type Small;
    typedef typename std::conditional<sizeof(Int) == 4, int16_t, int32_t>::type Medium;
    std::vector<Int> out(count);
    if (count == 0)
        return out;
    const size_t codeBytes = (count * 2 + 7) / 8;
    if (data.size() < sizeof(SInt) + codeBytes)
        throw std::runtime_error("USD: corrupted integer data!");
    SInt common;
    memcpy(&common, data.data(), sizeof(SInt));
    const uint8_t *codes = data.data() + sizeof(SInt);
    const uint8_t *vals = codes + codeBytes, *end = data.data() + data.size();
    SInt prev = 0;
    auto take = [&](size_t n, void *dst) {
        if ((size_t) (end - vals) < n)
            throw std::runtime_error("USD: corrupted integer data!");
        memcpy(dst, vals, n);
        vals += n;
    };
    for (size_t i = 0; i < count; ++i) {
        const int code = (codes[i / 4] >> (2 * (i % 4))) & 3;
        switch (code) {
            case 0: prev += common; break;
            case 1: { Small v; take(sizeof v, &v); prev += v; break; }
            case 2: { Medium v; take(sizeof v, &v); prev += v; break; }
            default: { SInt v; take(sizeof v, &v); prev += v; break; }
        }
        out[i] = (Int) prev;
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/*  File access                                                              */
/* ------------------------------------------------------------------------- */

CrateFile::CrateFile(const std::string &filename, uint64_t start, uint64_t size)
    : mFilename(filename), mStart(start), mSize(size) {
    mStream.open(filename, std::ios::binary);
    if (!mStream)
        throw std::runtime_error("Unable to open USD file \"" + filename + "\"!");
    if (mSize == 0) {
        mStream.seekg(0, std::ios::end);
        mSize = (uint64_t) mStream.tellg() - mStart;
    }
}

void CrateFile::read(uint64_t pos, uint64_t size, void *out) {
    if (pos > mSize || size > mSize - pos)
        fail("read past the end of the file");
    if (size == 0)
        return;
    mStream.clear();
    mStream.seekg((std::streamoff) (mStart + pos));
    mStream.read((char *) out, (std::streamsize) size);
    if (!mStream)
        fail("I/O error");
}

void CrateFile::fail(const std::string &msg) const {
    throw std::runtime_error("USD file \"" + mFilename + "\": " + msg + "!");
}

std::vector<uint8_t> CrateFile::bytes(uint64_t &pos, uint64_t size) {
    if (size > mSize)
        fail("block larger than the file");
    std::vector<uint8_t> b((size_t) size);
    read(pos, size, b.data());
    pos += size;
    return b;
}

template <typename Int>
std::vector<Int> CrateFile::compressed_ints(uint64_t &pos, uint64_t count) {
    const uint64_t compressedSize = get<uint64_t>(pos);
    if (count > mSize * 8)
        fail("integer array larger than the file allows");
    const std::vector<uint8_t> c = bytes(pos, compressedSize);
    if (count == 0)
        return std::vector<Int>();
    const size_t work = sizeof(Int) + (size_t) ((count * 2 + 7) / 8) + (size_t) count * sizeof(Int);
    return decode_ints<Int>(fast_decompress(c.data(), c.size(), work), (size_t) count);
}

/* ------------------------------------------------------------------------- */
/*  Structure                                                                */
/* ------------------------------------------------------------------------- */

void CrateFile::parse() {
    char ident[8];
    read(0, 8, ident);
    if (memcmp(ident, "PXR-USDC", 8) != 0)
        fail("not a .usdc file");
    uint8_t version[8];
    read(8, 8, version);
    mVersion = ((uint32_t) version[0] << 16) | ((uint32_t) version[1] << 8) | version[2];
    if (version[0] != 0 || mVersion > 0x000FFF)
        fail("unsupported Crate version " + std::to_string(version[0]) + "." + std::to_string(version[1]) +
             "." + std::to_string(version[2]));
    uint64_t tocOffset;
    read(16, 8, &tocOffset);

    uint64_t pos = tocOffset;
    const uint64_t numSections = get<uint64_t>(pos);
    if (numSections > 64)
        fail("invalid table of contents");
    struct Sec { uint64_t start = 0, size = 0; bool found = false; };
    std::map<std::string, Sec> sections;
    for (uint64_t k = 0; k < numSections; ++k) {
        char name[17] = { 0 };
        read(pos, 16, name);
        pos += 16;
        Sec sec;
        sec.start = get<uint64_t>(pos);
        sec.size = get<uint64_t>(pos);
        sec.found = true;
        sections[name] = sec;
        mSections.push_back(Section { name, sec.start, sec.size });
    }
    for (const char *name : { "TOKENS", "STRINGS", "FIELDS", "FIELDSETS", "PATHS", "SPECS" })
        if (!sections[name].found)
            fail(std::string("missing section ") + name);
    const bool compressed = mVersion >= 0x000400;

    /* TOKENS */
    pos = sections["TOKENS"].start;
    const uint64_t numTokens = get<uint64_t>(pos);
    std::vector<uint8_t> chars;
    if (!compressed) {
        const uint64_t n = get<uint64_t>(pos);
        chars = bytes(pos, n);
    } else {
        const uint64_t uncompressedSize = get<uint64_t>(pos), compressedSize = get<uint64_t>(pos);
        if (uncompressedSize > mSize * 256)
            fail("token table too large");
        const std::vector<uint8_t> c = bytes(pos, compressedSize);
        chars = fast_decompress(c.data(), c.size(), (size_t) uncompressedSize);
        if (chars.size() != uncompressedSize)
            fail("token table of an unexpected size");
    }
    mTokens.reserve((size_t) std::min<uint64_t>(numTokens, chars.size() + 1));
    for (size_t b = 0; mTokens.size() < numTokens;) {
        size_t e = b;
        while (e < chars.size() && chars[e] != 0)
            ++e;
        if (e >= chars.size() && mTokens.size() + 1 < numTokens)
            fail("token table truncated");
        mTokens.emplace_back((const char *) chars.data() + b, e - b);
        b = e + 1;
    }

    /* STRINGS: token index of every string */
    pos = sections["STRINGS"].start;
    {
        const uint64_t n = get<uint64_t>(pos);
        mStrings = vector_of<uint32_t>(pos, n);
    }

    /* FIELDS */
    pos = sections["FIELDS"].start;
    if (!compressed) {
        const uint64_t n = get<uint64_t>(pos);
        for (uint64_t k = 0; k < n; ++k) {
            Field f;
            pos += 4;   /* unused padding, first */
            f.token = get<uint32_t>(pos);
            f.rep = get<uint64_t>(pos);
            mFields.push_back(f);
        }
    } else {
        const uint64_t n = get<uint64_t>(pos);
        const std::vector<uint32_t> tokens = compressed_ints<uint32_t>(pos, n);
        const uint64_t repsSize = get<uint64_t>(pos);
        const std::vector<uint8_t> c = bytes(pos, repsSize);
        const std::vector<uint8_t> reps = fast_decompress(c.data(), c.size(), (size_t) n * 8);
        if (reps.size() != n * 8)
            fail("field table of an unexpected size");
        mFields.resize((size_t) n);
        for (size_t k = 0; k < n; ++k) {
            mFields[k].token = tokens[k];
            memcpy(&mFields[k].rep, reps.data() + 8 * k, 8);
        }
    }

    /* FIELDSETS */
    pos = sections["FIELDSETS"].start;
    {
        const uint64_t n = get<uint64_t>(pos);
        mFieldSets = compressed ? compressed_ints<uint32_t>(pos, n) : vector_of<uint32_t>(pos, n);
    }

    /* PATHS */
    pos = sections["PATHS"].start;
    {
        const uint64_t numPaths = get<uint64_t>(pos);
        if (numPaths > mSize)
            fail("too many paths");
        mPaths.assign((size_t) numPaths, std::string());
        if (compressed) {
            const uint64_t n = get<uint64_t>(pos);
            const std::vector<uint32_t> pathIndexes = compressed_ints<uint32_t>(pos, n);
            const std::vector<int32_t> elementTokens = compressed_ints<int32_t>(pos, n);
            const std::vector<int32_t> jumps = compressed_ints<int32_t>(pos, n);
            build_paths(pathIndexes, elementTokens, jumps);
            mPathTree = PathTree { pathIndexes, elementTokens, jumps };
        } else {
            old_paths(pos);
        }
    }

    /* SPECS */
    pos = sections["SPECS"].start;
    {
        const uint64_t n = get<uint64_t>(pos);
        if (compressed) {
            const std::vector<uint32_t> paths = compressed_ints<uint32_t>(pos, n);
            const std::vector<uint32_t> fieldSets = compressed_ints<uint32_t>(pos, n);
            const std::vector<uint32_t> types = compressed_ints<uint32_t>(pos, n);
            mSpecs.resize((size_t) n);
            for (size_t k = 0; k < n; ++k)
                mSpecs[k] = Spec { paths[k], fieldSets[k], types[k] };
        } else {
            for (uint64_t k = 0; k < n; ++k) {
                Spec sp;
                sp.path = get<uint32_t>(pos);
                sp.fieldSet = get<uint32_t>(pos);
                sp.type = get<uint32_t>(pos);
                mSpecs.push_back(sp);
            }
        }
    }
}

const std::string &CrateFile::token(uint64_t index) const {
    if (index >= mTokens.size())
        fail("invalid token index");
    return mTokens[(size_t) index];
}

const std::string &CrateFile::string(uint64_t index) const {
    if (index >= mStrings.size())
        fail("invalid string index");
    return token(mStrings[(size_t) index]);
}

const std::string &CrateFile::path(uint64_t index) const {
    if (index >= mPaths.size())
        fail("invalid path index");
    return mPaths[(size_t) index];
}

static std::string append_element(const std::string &parent, const std::string &element, bool property) {
    if (property)
        return parent + "." + element;
    if (!element.empty() && element[0] == '{')
        return parent + element;                 /* variant selection */
    if (parent == "/")
        return "/" + element;
    if (!parent.empty() && parent.back() == '}')
        return parent + element;                 /* child of a variant */
    return parent + "/" + element;
}

/* The compressed path tree (Pixar's _BuildDecompressedPathsImpl) */
void CrateFile::build_paths(const std::vector<uint32_t> &pathIndexes, const std::vector<int32_t> &elementTokens,
                            const std::vector<int32_t> &jumps) {
    struct Item { size_t index; std::string parent; };
    std::vector<Item> stack { Item { 0, std::string() } };
    const size_t n = pathIndexes.size();
    while (!stack.empty()) {
        Item it = stack.back();
        stack.pop_back();
        size_t cur = it.index;
        std::string parent = it.parent;
        bool hasChild, hasSibling;
        do {
            if (cur >= n || stack.size() > n)
                fail("corrupted path tree");
            const size_t self = cur++;
            if (pathIndexes[self] >= mPaths.size())
                fail("invalid path index");
            std::string &out = mPaths[pathIndexes[self]];
            if (parent.empty()) {
                parent = "/";
                out = "/";
            } else {
                const int32_t t = elementTokens[self];
                const uint32_t ti = (uint32_t) (t < 0 ? -(int64_t) t : t);
                out = append_element(parent, token(ti), t < 0);
            }
            const int32_t jump = jumps[self];
            hasChild = jump > 0 || jump == -1;
            hasSibling = jump >= 0;
            if (hasChild) {
                if (hasSibling) {
                    if ((uint64_t) self + (uint64_t) jump >= n)
                        fail("corrupted path tree");
                    stack.push_back(Item { self + (size_t) jump, parent });
                }
                parent = out;
            }
        } while (hasChild || hasSibling);
    }
}

/* Versions before 0.4.0: path items with headers (index, element token, bits) */
void CrateFile::old_paths(uint64_t &pos) {
    struct Header { uint32_t index; uint32_t elementToken; uint8_t bits; };
    std::vector<std::pair<uint64_t, std::string>> stack;   /* (sibling position, parent) */
    const bool v001 = mVersion < 0x000002;
    std::string parent;
    uint64_t cur = pos;
    std::vector<std::pair<uint64_t, std::string>> todo { { cur, std::string() } };
    while (!todo.empty()) {
        cur = todo.back().first;
        parent = todo.back().second;
        todo.pop_back();
        while (true) {
            Header h;
            h.index = get<uint32_t>(cur);
            h.elementToken = get<uint32_t>(cur);
            h.bits = get<uint8_t>(cur);
            if (!v001)
                cur += 3;   /* padding of the 12-byte header */
            const bool hasChild = h.bits & 1, hasSibling = h.bits & 2, isProperty = h.bits & 4;
            if (h.index >= mPaths.size())
                fail("invalid path index");
            std::string &out = mPaths[h.index];
            out = parent.empty() ? std::string("/") : append_element(parent, token(h.elementToken), isProperty);
            if (hasSibling && hasChild) {
                const int64_t siblingOffset = get<int64_t>(cur);
                todo.emplace_back((uint64_t) siblingOffset, parent);
            } else if (hasSibling && !hasChild) {
                /* the sibling follows */
                continue;
            }
            if (!hasChild)
                break;
            parent = out;
        }
    }
    (void) stack;
}

/* ------------------------------------------------------------------------- */
/*  Values                                                                   */
/* ------------------------------------------------------------------------- */

namespace {

enum Type {
    TInvalid = 0, TBool, TUChar, TInt, TUInt, TInt64, TUInt64, THalf, TFloat, TDouble, TString, TToken,
    TAssetPath, TMatrix2d, TMatrix3d, TMatrix4d, TQuatd, TQuatf, TQuath, TVec2d, TVec2f, TVec2h, TVec2i,
    TVec3d, TVec3f, TVec3h, TVec3i, TVec4d, TVec4f, TVec4h, TVec4i, TDictionary, TTokenListOp,
    TStringListOp, TPathListOp, TReferenceListOp, TIntListOp, TInt64ListOp, TUIntListOp, TUInt64ListOp,
    TPathVector, TTokenVector, TSpecifier, TPermission, TVariability, TVariantSelectionMap, TTimeSamples,
    TPayload, TDoubleVector, TLayerOffsetVector, TStringVector, TValueBlock, TValue, TUnregisteredValue,
    TUnregisteredValueListOp, TPayloadListOp, TTimeCode, TPathExpression
};

/* Scalar storage of the numeric types: bytes per component, components */
struct Numeric { int bytes; int count; char kind; };   /* kind: i(signed) u(unsigned) f(float) h(half) b(bool) */

bool numeric(int type, Numeric &n) {
    switch (type) {
        case TBool: n = { 1, 1, 'b' }; return true;
        case TUChar: n = { 1, 1, 'u' }; return true;
        case TInt: n = { 4, 1, 'i' }; return true;
        case TUInt: n = { 4, 1, 'u' }; return true;
        case TInt64: n = { 8, 1, 'i' }; return true;
        case TUInt64: n = { 8, 1, 'u' }; return true;
        case THalf: n = { 2, 1, 'h' }; return true;
        case TFloat: n = { 4, 1, 'f' }; return true;
        case TDouble: case TTimeCode: n = { 8, 1, 'f' }; return true;
        case TMatrix2d: n = { 8, 4, 'f' }; return true;
        case TMatrix3d: n = { 8, 9, 'f' }; return true;
        case TMatrix4d: n = { 8, 16, 'f' }; return true;
        case TQuatd: n = { 8, 4, 'f' }; return true;
        case TQuatf: n = { 4, 4, 'f' }; return true;
        case TQuath: n = { 2, 4, 'h' }; return true;
        case TVec2d: n = { 8, 2, 'f' }; return true;
        case TVec2f: n = { 4, 2, 'f' }; return true;
        case TVec2h: n = { 2, 2, 'h' }; return true;
        case TVec2i: n = { 4, 2, 'i' }; return true;
        case TVec3d: n = { 8, 3, 'f' }; return true;
        case TVec3f: n = { 4, 3, 'f' }; return true;
        case TVec3h: n = { 2, 3, 'h' }; return true;
        case TVec3i: n = { 4, 3, 'i' }; return true;
        case TVec4d: n = { 8, 4, 'f' }; return true;
        case TVec4f: n = { 4, 4, 'f' }; return true;
        case TVec4h: n = { 2, 4, 'h' }; return true;
        case TVec4i: n = { 4, 4, 'i' }; return true;
        default: return false;
    }
}

double half_to_double(uint16_t h) {
    const int sign = h >> 15, exp = (h >> 10) & 31, mant = h & 1023;
    double v;
    if (exp == 0)
        v = std::ldexp((double) mant, -24);
    else if (exp == 31)
        v = mant ? std::numeric_limits<double>::quiet_NaN() : std::numeric_limits<double>::infinity();
    else
        v = std::ldexp((double) (mant | 1024), exp - 25);
    return sign ? -v : v;
}

double scalar(const uint8_t *p, const Numeric &n) {
    switch (n.kind) {
        case 'b': return *p ? 1.0 : 0.0;
        case 'h': { uint16_t v; memcpy(&v, p, 2); return half_to_double(v); }
        case 'f':
            if (n.bytes == 4) { float v; memcpy(&v, p, 4); return v; }
            else { double v; memcpy(&v, p, 8); return v; }
        case 'i':
            if (n.bytes == 1) return (int8_t) *p;
            if (n.bytes == 4) { int32_t v; memcpy(&v, p, 4); return v; }
            { int64_t v; memcpy(&v, p, 8); return (double) v; }
        default:
            if (n.bytes == 1) return *p;
            if (n.bytes == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
            { uint64_t v; memcpy(&v, p, 8); return (double) v; }
    }
}

bool is_quat(int type) { return type == TQuatd || type == TQuatf || type == TQuath; }
bool is_matrix(int type) { return type == TMatrix2d || type == TMatrix3d || type == TMatrix4d; }

/* Gf quaternions are stored (x, y, z, w); USD text writes (w, x, y, z) */
void quat_to_text_order(std::vector<double> &v, size_t first) {
    const double w = v[first + 3];
    v[first + 3] = v[first + 2];
    v[first + 2] = v[first + 1];
    v[first + 1] = v[first];
    v[first] = w;
}

} // namespace

uint64_t CrateFile::count(uint64_t rep) {
    const bool isArray = (rep >> 63) & 1, inlined = (rep >> 62) & 1;
    const uint64_t payload = rep & ((1ull << 48) - 1);
    if (!isArray)
        return 1;
    if (inlined || payload == 0)
        return 0;
    uint64_t pos = payload;
    return array_size(pos);
}

Value CrateFile::value(uint64_t rep) {
    Value v;
    const bool isArray = (rep >> 63) & 1, inlined = (rep >> 62) & 1, compressedRep = (rep >> 61) & 1;
    const int type = (int) ((rep >> 48) & 0xff);
    const uint64_t payload = rep & ((1ull << 48) - 1);

    Numeric n;
    if (numeric(type, n)) {
        v.kind = Value::Numbers;
        v.tuple = n.count;
        v.array = isArray;
        if (!isArray && inlined) {
            /* scalars in the low 32 bits; vectors / matrices as int8 components
               (a matrix: its diagonal) */
            uint8_t raw[8];
            memcpy(raw, &payload, 8);
            if (n.count == 1) {
                if (n.kind == 'f' && n.bytes == 8) {
                    float f;
                    memcpy(&f, raw, 4);   /* a double that fits in a float */
                    v.numbers.push_back(f);
                } else if (n.kind == 'i' && n.bytes == 8) {
                    int32_t i;
                    memcpy(&i, raw, 4);
                    v.numbers.push_back(i);
                } else if (n.kind == 'u' && n.bytes == 8) {
                    uint32_t u;
                    memcpy(&u, raw, 4);
                    v.numbers.push_back(u);
                } else {
                    v.numbers.push_back(scalar(raw, n));
                }
            } else if (is_matrix(type)) {
                const int dim = type == TMatrix2d ? 2 : (type == TMatrix3d ? 3 : 4);
                v.numbers.assign((size_t) n.count, 0.0);
                for (int k = 0; k < dim; ++k)
                    v.numbers[(size_t) (k * dim + k)] = (int8_t) raw[k];
            } else {
                for (int k = 0; k < n.count; ++k)
                    v.numbers.push_back((int8_t) raw[k]);
                if (is_quat(type))
                    quat_to_text_order(v.numbers, 0);
            }
            return v;
        }
        uint64_t pos = payload;
        uint64_t count = 1;
        if (isArray) {
            if (payload == 0)
                return v;   /* an empty array */
            count = array_size(pos);
            if (count > mSize)
                fail("array larger than the file");
        }
        const size_t total = (size_t) count * (size_t) n.count;
        v.numbers.resize(total);
        if (isArray && compressedRep) {
            if (n.kind == 'i' || n.kind == 'u') {
                if (n.bytes == 8) {
                    const std::vector<int64_t> ints = compressed_ints<int64_t>(pos, count);
                    for (size_t k = 0; k < total; ++k)
                        v.numbers[k] = n.kind == 'i' ? (double) ints[k] : (double) (uint64_t) ints[k];
                } else {
                    const std::vector<int32_t> ints = compressed_ints<int32_t>(pos, count);
                    for (size_t k = 0; k < total; ++k)
                        v.numbers[k] = n.kind == 'i' ? (double) ints[k] : (double) (uint32_t) ints[k];
                }
            } else if (n.count == 1 && (n.kind == 'f' || n.kind == 'h')) {
                const uint8_t code = get<uint8_t>(pos);
                if (code == 'i') {
                    const std::vector<int32_t> ints = compressed_ints<int32_t>(pos, count);
                    for (size_t k = 0; k < total; ++k)
                        v.numbers[k] = ints[k];
                } else if (code == 't') {
                    const uint32_t lutSize = get<uint32_t>(pos);
                    const std::vector<uint8_t> lut = bytes(pos, (uint64_t) lutSize * (uint64_t) n.bytes);
                    const std::vector<uint32_t> idx = compressed_ints<uint32_t>(pos, count);
                    for (size_t k = 0; k < total; ++k) {
                        if (idx[k] >= lutSize)
                            fail("invalid lookup table index");
                        v.numbers[k] = scalar(lut.data() + (size_t) idx[k] * (size_t) n.bytes, n);
                    }
                } else {
                    fail("unknown float array compression");
                }
            } else {
                fail("unexpected compressed array");
            }
        } else {
            const std::vector<uint8_t> raw = bytes(pos, (uint64_t) total * (uint64_t) n.bytes);
            for (size_t k = 0; k < total; ++k)
                v.numbers[k] = scalar(raw.data() + k * (size_t) n.bytes, n);
            if (is_quat(type))
                for (size_t k = 0; k < count; ++k)
                    quat_to_text_order(v.numbers, k * 4);
        }
        return v;
    }

    switch (type) {
        case TToken: case TString: case TAssetPath: {
            v.kind = Value::Strings;
            v.array = isArray;
            auto text = [&](uint64_t index) { return type == TString ? string(index) : token(index); };
            if (!isArray) {
                if (inlined) {
                    v.strings.push_back(text(payload));
                } else {
                    uint64_t pos = payload;
                    v.strings.push_back(text(get<uint32_t>(pos)));
                }
            } else if (payload != 0) {
                uint64_t pos = payload;
                const uint64_t count = array_size(pos);
                for (uint32_t index : vector_of<uint32_t>(pos, count))
                    v.strings.push_back(text(index));
            }
            return v;
        }
        case TTokenVector: case TPathVector: case TStringVector: {
            v.kind = Value::Strings;
            v.array = true;
            uint64_t pos = payload;
            const uint64_t count = get<uint64_t>(pos);
            for (uint32_t index : vector_of<uint32_t>(pos, count))
                v.strings.push_back(type == TTokenVector ? token(index)
                                    : type == TPathVector ? path(index) : string(index));
            return v;
        }
        case TDoubleVector: {
            v.kind = Value::Numbers;
            v.array = true;
            uint64_t pos = payload;
            const uint64_t count = get<uint64_t>(pos);
            v.numbers = vector_of<double>(pos, count);
            return v;
        }
        case TSpecifier: case TPermission: case TVariability: {
            v.kind = Value::Numbers;
            v.numbers.push_back((double) (uint32_t) payload);
            return v;
        }
        case TValueBlock:
            v.kind = Value::Blocked;
            return v;
        case TTimeSamples: {
            /* a jump (relative to itself) to the rep of the times; after that
               rep, a jump to the value count followed by the value reps */
            v.kind = Value::TimeSamples;
            uint64_t pos = payload;
            const int64_t toTimes = get<int64_t>(pos);
            pos = payload + (uint64_t) toTimes;
            const uint64_t timesRep = get<uint64_t>(pos);
            if (((timesRep >> 48) & 0xff) == TTimeSamples)
                fail("invalid time samples");
            uint64_t valuesAt = pos;
            const int64_t toValues = get<int64_t>(valuesAt);
            valuesAt = pos + (uint64_t) toValues;
            const uint64_t count = get<uint64_t>(valuesAt);
            if (count > mSize / 8)
                fail("invalid time samples");
            const std::vector<uint64_t> reps = vector_of<uint64_t>(valuesAt, count);
            const Value times = value(timesRep);
            if (times.numbers.size() != count)
                fail("time samples: times and values do not match");
            for (size_t k = 0; k < count; ++k) {
                if (((reps[k] >> 48) & 0xff) == TTimeSamples)
                    fail("invalid time samples");
                v.samples.emplace_back(times.numbers[k], value(reps[k]));
            }
            return v;
        }
        case TTokenListOp: case TPathListOp: case TStringListOp: {
            v.kind = Value::ListOp;
            uint64_t pos = payload;
            const uint8_t header = get<uint8_t>(pos);
            v.isExplicit = header & 1;
            auto items = [&]() {
                const uint64_t count = get<uint64_t>(pos);
                std::vector<std::string> out;
                for (uint32_t index : vector_of<uint32_t>(pos, count))
                    out.push_back(type == TTokenListOp ? token(index)
                                  : type == TPathListOp ? path(index) : string(index));
                return out;
            };
            /* order of the lists: explicit, added, prepended, appended, deleted, ordered */
            if (header & 2) v.explicitItems = items();
            if (header & 4) v.appended = items();
            if (header & 32) v.prepended = items();
            if (header & 64) {
                const std::vector<std::string> a = items();
                v.appended.insert(v.appended.end(), a.begin(), a.end());
            }
            if (header & 8) v.deleted = items();
            return v;
        }
        case TReferenceListOp: case TPayloadListOp: case TPayload: {
            /* SdfReference: asset (string index), prim path (path index),
               layer offset (2 doubles), custom data (count, then string
               index + 8-byte value jump each). SdfPayload: no custom data,
               a layer offset from 0.8.0. Items as the .usda reader keeps
               them: "asset<path>", "<path>" or "asset" */
            const bool reference = type == TReferenceListOp;
            uint64_t pos = payload;
            auto item = [&]() {
                std::string asset = string(get<uint32_t>(pos));
                const std::string prim = path(get<uint32_t>(pos));
                if (reference || mVersion >= 0x000800) {
                    get<double>(pos);
                    get<double>(pos);
                }
                if (reference) {
                    const uint64_t n = get<uint64_t>(pos);
                    if (n > mSize / 12)
                        fail("invalid reference custom data");
                    for (uint64_t k = 0; k < n; ++k) {
                        get<uint32_t>(pos);
                        get<int64_t>(pos);
                    }
                }
                if (!prim.empty())
                    asset += "<" + prim + ">";
                return asset;
            };
            v.kind = Value::ListOp;
            if (type == TPayload) {
                v.isExplicit = true;
                v.explicitItems.push_back(item());
                return v;
            }
            const uint8_t header = get<uint8_t>(pos);
            v.isExplicit = header & 1;
            auto items = [&]() {
                const uint64_t count = get<uint64_t>(pos);
                if (count > mSize / 8)
                    fail("invalid reference list");
                std::vector<std::string> out;
                for (uint64_t k = 0; k < count; ++k)
                    out.push_back(item());
                return out;
            };
            if (header & 2) v.explicitItems = items();
            if (header & 4) v.appended = items();
            if (header & 32) v.prepended = items();
            if (header & 64) {
                const std::vector<std::string> a = items();
                v.appended.insert(v.appended.end(), a.begin(), a.end());
            }
            if (header & 8) v.deleted = items();
            return v;
        }
        case TVariantSelectionMap: {
            v.kind = Value::Dictionary;
            uint64_t pos = payload;
            const uint64_t count = get<uint64_t>(pos);
            for (uint64_t k = 0; k < count && k < mSize; ++k) {
                const std::string key = string(get<uint32_t>(pos));
                Value sel;
                sel.kind = Value::Strings;
                sel.strings.push_back(string(get<uint32_t>(pos)));
                v.dict[key] = sel;
            }
            return v;
        }
        default:
            v.kind = Value::Unsupported;
            return v;
    }
}

} // namespace usd
