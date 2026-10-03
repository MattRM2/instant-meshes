/*
    usdcwrite.cpp: Crate (.usdc) and package (.usdz) writing (see usdcwrite.h)

    The inverse of usdc.cpp for version 0.8.0: a bootstrap (ident, version,
    table of contents offset), the values, then the sections TOKENS,
    STRINGS, FIELDS, FIELDSETS, PATHS, SPECS and the table of contents.
    Value reps: bit 63 array, 62 inlined, 61 compressed, type in bits 48-55,
    payload (the value itself when inlined, else its file offset).
*/

#include "usdcwrite.h"
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <set>

namespace usd {

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

enum SpecType { SpecAttribute = 1, SpecPrim = 6, SpecPseudoRoot = 7, SpecRelationship = 8 };

const uint64_t ArrayBit = 1ull << 63, InlinedBit = 1ull << 62;

uint64_t rep_of(int type, bool array, bool inlined, uint64_t payload) {
    return (array ? ArrayBit : 0) | (inlined ? InlinedBit : 0) | ((uint64_t) type << 48) | (payload & ((1ull << 48) - 1));
}

/* Storage of a USD value type: crate type, components, bytes per component,
   kind (f float, d double, h half, i/I signed 32/64, u/U unsigned 32/64,
   b bool, c uchar, s token / string / asset) */
struct Storage {
    int type = TInvalid;
    int count = 1, bytes = 4;
    char kind = 'f';
    bool quat = false;
};

bool storage_of(std::string name, Storage &s) {
    if (name.size() > 2 && name.compare(name.size() - 2, 2, "[]") == 0)
        name.resize(name.size() - 2);
    struct Simple { const char *name; Storage s; };
    static const Simple simple[] = {
        { "bool", { TBool, 1, 1, 'b' } }, { "uchar", { TUChar, 1, 1, 'c' } }, { "int", { TInt, 1, 4, 'i' } },
        { "uint", { TUInt, 1, 4, 'u' } }, { "int64", { TInt64, 1, 8, 'I' } }, { "uint64", { TUInt64, 1, 8, 'U' } },
        { "half", { THalf, 1, 2, 'h' } }, { "float", { TFloat, 1, 4, 'f' } }, { "double", { TDouble, 1, 8, 'd' } },
        { "timecode", { TTimeCode, 1, 8, 'd' } }, { "string", { TString, 1, 4, 's' } },
        { "token", { TToken, 1, 4, 's' } }, { "asset", { TAssetPath, 1, 4, 's' } },
        { "matrix2d", { TMatrix2d, 4, 8, 'd' } }, { "matrix3d", { TMatrix3d, 9, 8, 'd' } },
        { "matrix4d", { TMatrix4d, 16, 8, 'd' } }, { "frame4d", { TMatrix4d, 16, 8, 'd' } },
        { "quatd", { TQuatd, 4, 8, 'd', true } }, { "quatf", { TQuatf, 4, 4, 'f', true } },
        { "quath", { TQuath, 4, 2, 'h', true } },
    };
    for (const Simple &e : simple)
        if (name == e.name) {
            s = e.s;
            return true;
        }
    /* vectors: float3, double2, half4, int3, and the roles point3f,
       normal3f, vector3d, color4f, texCoord2h... */
    char scalar = 0;
    int n = 0;
    for (const char *prefix : { "float", "double", "half", "int" })
        if (name.size() == strlen(prefix) + 1 && name.compare(0, strlen(prefix), prefix) == 0) {
            n = name.back() - '0';
            scalar = prefix[0];
        }
    if (!scalar && name.size() >= 3) {
        const char last = name.back(), digit = name[name.size() - 2];
        if ((last == 'f' || last == 'd' || last == 'h') && digit >= '2' && digit <= '4') {
            const std::string role = name.substr(0, name.size() - 2);
            for (const char *r : { "point", "normal", "vector", "color", "texCoord" })
                if (role == r) {
                    n = digit - '0';
                    scalar = last;
                }
        }
    }
    if (!scalar || n < 2 || n > 4)
        return false;
    static const int ids[4][3] = { { TVec2f, TVec3f, TVec4f }, { TVec2d, TVec3d, TVec4d },
                                   { TVec2h, TVec3h, TVec4h }, { TVec2i, TVec3i, TVec4i } };
    const int row = scalar == 'f' ? 0 : scalar == 'd' ? 1 : scalar == 'h' ? 2 : 3;
    s.type = ids[row][n - 2];
    s.count = n;
    s.bytes = scalar == 'd' ? 8 : scalar == 'h' ? 2 : 4;
    s.kind = scalar == 'i' ? 'i' : scalar;
    return true;
}

uint16_t float_to_half(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000;
    const int32_t exp = (int32_t) ((x >> 23) & 0xff) - 127 + 15;
    uint32_t mant = x & 0x7fffff;
    if (((x >> 23) & 0xff) == 0xff)
        return (uint16_t) (sign | 0x7c00 | (mant ? 0x200 : 0));
    if (exp >= 31)
        return (uint16_t) (sign | 0x7c00);
    if (exp <= 0) {
        if (exp < -10)
            return (uint16_t) sign;
        mant |= 0x800000;
        const int shift = 14 - exp;
        uint32_t h = mant >> shift;
        if ((mant >> (shift - 1)) & 1)
            ++h;
        return (uint16_t) (sign | h);
    }
    uint32_t h = sign | ((uint32_t) exp << 10) | (mant >> 13);
    if (mant & 0x1000)
        ++h;   /* round half up (carries into the exponent when needed) */
    return (uint16_t) h;
}

/* Metadata types (the keys the text reader keeps without a type) */
int meta_type(const std::string &key) {
    static const std::map<std::string, int> types {
        { "kind", TToken }, { "upAxis", TToken }, { "defaultPrim", TToken }, { "interpolation", TToken },
        { "doc", TString }, { "comment", TString }, { "documentation", TString },
        { "active", TBool }, { "instanceable", TBool }, { "hidden", TBool },
        { "metersPerUnit", TDouble }, { "kilogramsPerUnit", TDouble }, { "startTimeCode", TDouble },
        { "endTimeCode", TDouble }, { "timeCodesPerSecond", TDouble }, { "framesPerSecond", TDouble },
        { "elementSize", TInt }, { "apiSchemas", TTokenListOp }, { "subLayers", TStringVector },
    };
    auto it = types.find(key);
    return it == types.end() ? TInvalid : it->second;
}

class CrateWriter {
public:
    std::vector<uint8_t> run(const std::map<std::string, Value> &meta, const Prim &root) {
        mOut.assign(88, 0);                /* bootstrap */
        memcpy(mOut.data(), "PXR-USDC", 8);
        mOut[9] = 8;                       /* version 0.8.0 */
        token("");                         /* index 0: never a path element */
        path("/");

        /* the pseudo-root, then the prims in order */
        std::vector<std::pair<uint32_t, uint64_t>> fields;
        for (const auto &kv : meta)
            meta_field(kv.first, kv.second, fields);
        if (!root.children.empty())
            fields.emplace_back(token("primChildren"), names(root.children));
        spec("/", SpecPseudoRoot, fields);
        for (const auto &c : root.children)
            prim(*c);

        /* sections */
        std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> toc;
        auto section = [&](const char *name, std::function<void()> body) {
            align();
            const uint64_t start = mOut.size();
            body();
            toc.push_back(std::make_pair(std::string(name), std::make_pair(start, (uint64_t) mOut.size() - start)));
        };
        section("TOKENS", [&] {
            std::vector<uint8_t> chars;
            for (const std::string &t : mTokens) {
                chars.insert(chars.end(), t.begin(), t.end());
                chars.push_back(0);
            }
            put<uint64_t>(mTokens.size());
            put<uint64_t>(chars.size());
            const std::vector<uint8_t> c = compress(chars);
            put<uint64_t>(c.size());
            bytes(c);
        });
        section("STRINGS", [&] {
            put<uint64_t>(mStrings.size());
            for (uint32_t s : mStrings)
                put<uint32_t>(s);
        });
        section("FIELDS", [&] {
            put<uint64_t>(mFields.size());
            std::vector<int32_t> tokens;
            std::vector<uint8_t> reps(mFields.size() * 8);
            for (size_t k = 0; k < mFields.size(); ++k) {
                tokens.push_back((int32_t) mFields[k].first);
                memcpy(reps.data() + 8 * k, &mFields[k].second, 8);
            }
            ints(tokens);
            const std::vector<uint8_t> c = compress(reps);
            put<uint64_t>(c.size());
            bytes(c);
        });
        section("FIELDSETS", [&] {
            put<uint64_t>(mFieldSets.size());
            ints(std::vector<int32_t>(mFieldSets.begin(), mFieldSets.end()));
        });
        section("PATHS", [&] { paths(); });
        section("SPECS", [&] {
            put<uint64_t>(mSpecs.size());
            std::vector<int32_t> p, f, t;
            for (const auto &s : mSpecs) {
                p.push_back((int32_t) s[0]);
                f.push_back((int32_t) s[1]);
                t.push_back((int32_t) s[2]);
            }
            ints(p);
            ints(f);
            ints(t);
        });
        align();
        const uint64_t tocOffset = mOut.size();
        put<uint64_t>(toc.size());
        for (const auto &s : toc) {
            char name[16] = { 0 };
            memcpy(name, s.first.c_str(), s.first.size());
            mOut.insert(mOut.end(), name, name + 16);
            put<uint64_t>(s.second.first);
            put<uint64_t>(s.second.second);
        }
        memcpy(mOut.data() + 16, &tocOffset, 8);
        return std::move(mOut);
    }

private:
    template <typename T> void put(T v) {
        const size_t at = mOut.size();
        mOut.resize(at + sizeof(T));
        memcpy(mOut.data() + at, &v, sizeof(T));
    }
    void bytes(const std::vector<uint8_t> &b) { mOut.insert(mOut.end(), b.begin(), b.end()); }
    void align() {
        while (mOut.size() % 8)
            mOut.push_back(0);
    }
    static std::vector<uint8_t> compress(const std::vector<uint8_t> &data) {
        std::vector<uint8_t> c { 0 };   /* TfFastCompression: one chunk */
        const std::vector<uint8_t> block = lz4_literals(data.data(), data.size());
        c.insert(c.end(), block.begin(), block.end());
        return c;
    }
    void ints(const std::vector<int32_t> &values) {
        const std::vector<uint8_t> c = compress(encode_ints(values));
        put<uint64_t>(c.size());
        bytes(c);
    }

    uint32_t token(const std::string &t) {
        auto it = mTokenIds.find(t);
        if (it != mTokenIds.end())
            return it->second;
        mTokens.push_back(t);
        return mTokenIds[t] = (uint32_t) (mTokens.size() - 1);
    }
    uint32_t string(const std::string &s) {
        auto it = mStringIds.find(s);
        if (it != mStringIds.end())
            return it->second;
        mStrings.push_back(token(s));
        return mStringIds[s] = (uint32_t) (mStrings.size() - 1);
    }
    /* A path and its ancestors; prim paths "/A/B", property paths "/A/B.p" */
    uint32_t path(const std::string &p) {
        auto it = mPathIds.find(p);
        if (it != mPathIds.end())
            return it->second;
        if (p != "/") {
            const size_t dot = p.find('.', p.rfind('/'));
            const size_t cut = dot != std::string::npos ? dot : p.rfind('/');
            const std::string parent = cut == 0 ? std::string("/") : p.substr(0, cut);
            path(parent);
            mChildren[parent].push_back(p);
            mElements[p] = dot != std::string::npos ? -(int32_t) token(p.substr(dot + 1))
                                                    : (int32_t) token(p.substr(cut + 1));
        }
        mPaths.push_back(p);
        return mPathIds[p] = (uint32_t) (mPaths.size() - 1);
    }

    /* The path tree in pre-order: index, element token (negative for a
       property), jump (-1 child only, 0 sibling only, n > 0 both: the
       sibling n entries further, -2 neither) */
    void paths() {
        std::vector<int32_t> indexes, elements, jumps;
        std::function<void(const std::string &, bool)> visit = [&](const std::string &p, bool sibling) {
            const size_t self = indexes.size();
            indexes.push_back((int32_t) mPathIds.at(p));
            elements.push_back(p == "/" ? 0 : mElements.at(p));
            jumps.push_back(0);
            const auto it = mChildren.find(p);
            const bool child = it != mChildren.end() && !it->second.empty();
            if (child) {
                const std::vector<std::string> &kids = it->second;
                for (size_t k = 0; k < kids.size(); ++k)
                    visit(kids[k], k + 1 < kids.size());
            }
            jumps[self] = child && sibling ? (int32_t) (indexes.size() - self) : child ? -1 : sibling ? 0 : -2;
        };
        visit("/", false);
        put<uint64_t>(mPaths.size());
        put<uint64_t>(indexes.size());
        ints(indexes);
        ints(elements);
        ints(jumps);
    }

    void spec(const std::string &p, int type, const std::vector<std::pair<uint32_t, uint64_t>> &fields) {
        const uint32_t set = (uint32_t) mFieldSets.size();
        for (const auto &f : fields) {
            mFields.push_back(f);
            mFieldSets.push_back((uint32_t) (mFields.size() - 1));
        }
        mFieldSets.push_back(0xffffffffu);
        mSpecs.push_back({ path(p), set, (uint32_t) type });
    }

    /* Non-inlined data: its offset (8-byte aligned) */
    uint64_t here() {
        align();
        return mOut.size();
    }

    uint64_t index_list(int type, const std::vector<uint32_t> &items) {
        if (items.empty() && type != TTokenVector && type != TStringVector && type != TPathVector)
            return rep_of(type, false, false, 0);
        const uint64_t at = here();
        put<uint64_t>(items.size());
        for (uint32_t i : items)
            put<uint32_t>(i);
        return rep_of(type, false, false, at);
    }

    uint64_t names(const std::vector<std::unique_ptr<Prim>> &children) {
        std::vector<uint32_t> ids;
        for (const auto &c : children)
            ids.push_back(token(c->name));
        return index_list(TTokenVector, ids);
    }

    uint64_t list_op(int type, const Value &v) {
        auto ids = [&](const std::vector<std::string> &items) {
            std::vector<uint32_t> out;
            for (const std::string &i : items)
                out.push_back(type == TPathListOp ? path(i) : type == TStringListOp ? string(i) : token(i));
            return out;
        };
        uint8_t header = v.isExplicit ? 1 : 0;
        if (!v.explicitItems.empty()) header |= 2;
        if (!v.prepended.empty()) header |= 32;
        if (!v.appended.empty()) header |= 64;
        if (!v.deleted.empty()) header |= 8;
        const uint64_t at = here();
        put<uint8_t>(header);
        for (const std::vector<std::string> *list : { &v.explicitItems, &v.prepended, &v.appended, &v.deleted }) {
            if (list->empty())
                continue;
            const std::vector<uint32_t> i = ids(*list);
            put<uint64_t>(i.size());
            for (uint32_t x : i)
                put<uint32_t>(x);
        }
        return rep_of(type, false, false, at);
    }

    /* One element of a numeric value at 'at' in 'v.numbers' */
    void element(const Storage &s, const std::vector<double> &n, size_t at) {
        for (int k = 0; k < s.count; ++k) {
            /* quaternions: text order (w, x, y, z), stored (x, y, z, w) */
            const size_t src = s.quat ? at + (size_t) ((k + 1) % 4) : at + (size_t) k;
            const double x = src < n.size() ? n[src] : 0;
            switch (s.kind) {
                case 'b': case 'c': put<uint8_t>((uint8_t) (x != 0 && s.kind == 'b' ? 1 : x)); break;
                case 'h': put<uint16_t>(float_to_half((float) x)); break;
                case 'f': put<float>((float) x); break;
                case 'd': put<double>(x); break;
                case 'i': put<int32_t>((int32_t) x); break;
                case 'u': put<uint32_t>((uint32_t) x); break;
                case 'I': put<int64_t>((int64_t) x); break;
                default: put<uint64_t>((uint64_t) x); break;
            }
        }
    }

    uint64_t value(const Value &v, const std::string &typeName, const std::string &where) {
        if (v.kind == Value::Blocked)
            return rep_of(TValueBlock, false, true, 0);
        Storage s;
        if (!storage_of(typeName, s))
            throw std::runtime_error(".usdc writer: unsupported type \"" + typeName + "\" (" + where + ")");
        const bool array = typeName.size() > 2 && typeName.compare(typeName.size() - 2, 2, "[]") == 0;
        if (s.kind == 's') {
            auto id = [&](const std::string &t) { return s.type == TString ? string(t) : token(t); };
            if (!array)
                return rep_of(s.type, false, true, id(v.str()));
            if (v.strings.empty())
                return rep_of(s.type, true, false, 0);
            const uint64_t at = here();
            put<uint64_t>(v.strings.size());
            for (const std::string &t : v.strings)
                put<uint32_t>(id(t));
            return rep_of(s.type, true, false, at);
        }
        if (v.kind != Value::Numbers)
            throw std::runtime_error(".usdc writer: \"" + where + "\" has no numeric value");
        if (!array) {
            const double x = v.num();
            if (s.count == 1 && (s.kind == 'b' || s.kind == 'c' || s.kind == 'i' || s.kind == 'u' || s.kind == 'f')) {
                uint64_t payload = 0;
                if (s.kind == 'f') {
                    const float f = (float) x;
                    uint32_t bits;
                    memcpy(&bits, &f, 4);
                    payload = bits;
                } else if (s.kind == 'i') {
                    payload = (uint32_t) (int32_t) x;
                } else {
                    payload = (uint64_t) (uint32_t) x;
                }
                return rep_of(s.type, false, true, payload);
            }
            const uint64_t at = here();
            element(s, v.numbers, 0);
            return rep_of(s.type, false, false, at);
        }
        const size_t count = v.numbers.size() / (size_t) s.count;
        if (count == 0)
            return rep_of(s.type, true, false, 0);
        const uint64_t at = here();
        put<uint64_t>(count);
        for (size_t k = 0; k < count; ++k)
            element(s, v.numbers, k * (size_t) s.count);
        return rep_of(s.type, true, false, at);
    }

    uint64_t time_samples(const Value &v, const std::string &typeName, const std::string &where) {
        std::vector<uint64_t> reps;
        for (const auto &sample : v.samples)
            reps.push_back(value(sample.second, typeName, where));
        const uint64_t timesAt = here();
        put<uint64_t>(v.samples.size());
        for (const auto &sample : v.samples)
            put<double>(sample.first);
        const uint64_t timesRep = rep_of(TDoubleVector, false, false, timesAt);
        /* a jump to the times rep, the rep, a jump to the count and value reps */
        const uint64_t at = here();
        put<int64_t>(8);
        put<uint64_t>(timesRep);
        put<int64_t>(8);
        put<uint64_t>(reps.size());
        for (uint64_t r : reps)
            put<uint64_t>(r);
        return rep_of(TTimeSamples, false, false, at);
    }

    void meta_field(const std::string &key, const Value &v, std::vector<std::pair<uint32_t, uint64_t>> &fields) {
        if (key == "__samples__")
            return;
        const int type = meta_type(key);
        uint64_t rep;
        switch (type) {
            case TToken: rep = rep_of(TToken, false, true, token(v.str())); break;
            case TString: rep = rep_of(TString, false, true, string(v.str())); break;
            case TBool: rep = rep_of(TBool, false, true, v.num() != 0 ? 1 : 0); break;
            case TInt: rep = rep_of(TInt, false, true, (uint32_t) (int32_t) v.num()); break;
            case TDouble: {
                const uint64_t at = here();
                put<double>(v.num());
                rep = rep_of(TDouble, false, false, at);
                break;
            }
            case TTokenListOp: rep = list_op(TTokenListOp, v); break;
            case TStringVector: {
                std::vector<uint32_t> ids;
                for (const std::string &s : v.strings)
                    ids.push_back(string(s));
                rep = index_list(TStringVector, ids);
                if (key == "subLayers") {
                    /* USD reads one layer offset per sublayer: (offset 0, scale 1) */
                    const uint64_t at = here();
                    put<uint64_t>(v.strings.size());
                    for (size_t k = 0; k < v.strings.size(); ++k) {
                        put<double>(0.0);
                        put<double>(1.0);
                    }
                    fields.emplace_back(token("subLayerOffsets"), rep_of(TLayerOffsetVector, false, false, at));
                }
                break;
            }
            default:
                if (mSkipped.insert(key).second)
                    cout << "Warning: .usdc writer: metadata \"" << key << "\" not written" << endl;
                return;
        }
        fields.emplace_back(token(key), rep);
    }

    void prim(const Prim &p) {
        if (!p.variants.empty())
            throw std::runtime_error(".usdc writer: variants are not written (\"" + p.path + "\")");
        std::vector<std::pair<uint32_t, uint64_t>> fields;
        const uint32_t specifier = p.specifier == Specifier::Over ? 1 : p.specifier == Specifier::Class ? 2 : 0;
        fields.emplace_back(token("specifier"), rep_of(TSpecifier, false, true, specifier));
        if (!p.type.empty())
            fields.emplace_back(token("typeName"), rep_of(TToken, false, true, token(p.type)));
        for (const auto &kv : p.meta)
            meta_field(kv.first, kv.second, fields);
        if (!p.properties.empty()) {
            std::vector<uint32_t> ids;
            for (const Property &q : p.properties)
                ids.push_back(token(q.name));
            fields.emplace_back(token("properties"), index_list(TTokenVector, ids));
        }
        if (!p.children.empty())
            fields.emplace_back(token("primChildren"), names(p.children));
        spec(p.path, SpecPrim, fields);

        for (const Property &q : p.properties) {
            const std::string qpath = p.path + "." + q.name;
            std::vector<std::pair<uint32_t, uint64_t>> f;
            if (q.custom)
                f.emplace_back(token("custom"), rep_of(TBool, false, true, 1));
            if (q.relationship) {
                Value targets;
                targets.kind = Value::ListOp;
                targets.isExplicit = true;
                targets.explicitItems = q.targets;
                f.emplace_back(token("targetPaths"), list_op(TPathListOp, targets));
                spec(qpath, SpecRelationship, f);
                continue;
            }
            f.emplace_back(token("typeName"), rep_of(TToken, false, true, token(q.type)));
            if (q.uniform)
                f.emplace_back(token("variability"), rep_of(TVariability, false, true, 1));
            if (q.value.kind != Value::Empty)
                f.emplace_back(token("default"), value(q.value, q.type, qpath));
            auto samples = q.meta.find("__samples__");
            if (samples != q.meta.end())
                f.emplace_back(token("timeSamples"), time_samples(samples->second, q.type, qpath));
            if (!q.targets.empty()) {
                Value c;
                c.kind = Value::ListOp;
                c.isExplicit = true;
                c.explicitItems = q.targets;
                f.emplace_back(token("connectionPaths"), list_op(TPathListOp, c));
            }
            for (const auto &kv : q.meta)
                meta_field(kv.first, kv.second, f);
            spec(qpath, SpecAttribute, f);
        }
        for (const auto &c : p.children)
            prim(*c);
    }

    std::vector<uint8_t> mOut;
    std::vector<std::string> mTokens;
    std::map<std::string, uint32_t> mTokenIds, mStringIds, mPathIds;
    std::vector<uint32_t> mStrings;
    std::vector<std::string> mPaths;
    std::map<std::string, std::vector<std::string>> mChildren;
    std::map<std::string, int32_t> mElements;
    std::vector<std::pair<uint32_t, uint64_t>> mFields;
    std::vector<uint32_t> mFieldSets;
    std::vector<std::array<uint32_t, 3>> mSpecs;
    std::set<std::string> mSkipped;
};

uint32_t crc32(const std::vector<uint8_t> &data) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xffffffffu;
    for (uint8_t b : data)
        c = table[(c ^ b) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

} // namespace

std::vector<uint8_t> lz4_literals(const uint8_t *data, size_t size) {
    std::vector<uint8_t> out;
    out.reserve(size + size / 255 + 16);
    if (size < 15) {
        out.push_back((uint8_t) (size << 4));
    } else {
        out.push_back(0xF0);
        size_t rest = size - 15;
        while (rest >= 255) {
            out.push_back(255);
            rest -= 255;
        }
        out.push_back((uint8_t) rest);
    }
    out.insert(out.end(), data, data + size);
    return out;
}

std::vector<uint8_t> encode_ints(const std::vector<int32_t> &values) {
    std::vector<int32_t> deltas(values.size());
    int64_t prev = 0;
    std::map<int32_t, size_t> counts;
    for (size_t i = 0; i < values.size(); ++i) {
        deltas[i] = (int32_t) (uint32_t) ((int64_t) values[i] - prev);
        prev = values[i];
        counts[deltas[i]]++;
    }
    int32_t common = 0;
    size_t most = 0;
    for (const auto &kv : counts)
        if (kv.second > most) {
            most = kv.second;
            common = kv.first;
        }
    std::vector<uint8_t> out(4 + (values.size() * 2 + 7) / 8, 0);
    memcpy(out.data(), &common, 4);
    for (size_t i = 0; i < deltas.size(); ++i) {
        const int32_t d = deltas[i];
        int code;
        if (d == common) {
            code = 0;
        } else if (d >= -128 && d <= 127) {
            code = 1;
            out.push_back((uint8_t) (int8_t) d);
        } else if (d >= -32768 && d <= 32767) {
            code = 2;
            const int16_t v = (int16_t) d;
            out.insert(out.end(), (const uint8_t *) &v, (const uint8_t *) &v + 2);
        } else {
            code = 3;
            out.insert(out.end(), (const uint8_t *) &d, (const uint8_t *) &d + 4);
        }
        out[4 + i / 4] |= (uint8_t) (code << (2 * (i % 4)));
    }
    return out;
}

std::vector<uint8_t> encode_crate(const std::map<std::string, Value> &meta, const Prim &root) {
    return CrateWriter().run(meta, root);
}

std::vector<uint8_t> encode_usdz(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &files) {
    std::vector<uint8_t> out, central;
    auto put16 = [](std::vector<uint8_t> &v, uint32_t x) { v.push_back((uint8_t) x); v.push_back((uint8_t) (x >> 8)); };
    auto put32 = [&](std::vector<uint8_t> &v, uint32_t x) { put16(v, x & 0xffff); put16(v, x >> 16); };
    for (const auto &f : files) {
        if (f.second.size() >= 0xffffffffu || out.size() >= 0xffffffffu)
            throw std::runtime_error(".usdz writer: package larger than 4 GB");
        const uint32_t crc = crc32(f.second), offset = (uint32_t) out.size();
        /* data on a 64-byte boundary: an extra field pads the header */
        size_t pad = (64 - (out.size() + 30 + f.first.size()) % 64) % 64;
        if (pad > 0 && pad < 4)
            pad += 64;
        std::vector<uint8_t> header;
        put32(header, 0x04034b50);
        put16(header, 20);
        put16(header, 0);
        put16(header, 0);                  /* stored */
        put16(header, 0);
        put16(header, 0x21);               /* 1980-01-01 */
        put32(header, crc);
        put32(header, (uint32_t) f.second.size());
        put32(header, (uint32_t) f.second.size());
        put16(header, (uint32_t) f.first.size());
        put16(header, (uint32_t) pad);
        header.insert(header.end(), f.first.begin(), f.first.end());
        if (pad > 0) {
            put16(header, 0x1986);         /* padding field */
            put16(header, (uint32_t) (pad - 4));
            header.resize(header.size() + pad - 4, 0);
        }
        out.insert(out.end(), header.begin(), header.end());
        out.insert(out.end(), f.second.begin(), f.second.end());

        put32(central, 0x02014b50);
        put16(central, 20);
        put16(central, 20);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0x21);
        put32(central, crc);
        put32(central, (uint32_t) f.second.size());
        put32(central, (uint32_t) f.second.size());
        put16(central, (uint32_t) f.first.size());
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put16(central, 0);
        put32(central, 0);
        put32(central, offset);
        central.insert(central.end(), f.first.begin(), f.first.end());
    }
    const uint32_t centralAt = (uint32_t) out.size();
    out.insert(out.end(), central.begin(), central.end());
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, (uint32_t) files.size());
    put16(out, (uint32_t) files.size());
    put32(out, (uint32_t) central.size());
    put32(out, centralAt);
    put16(out, 0);
    return out;
}

void write_layer_file(const std::string &filename, const std::string &usdaText, const std::string &output) {
    const size_t dot = output.rfind('.');
    const std::string ext = dot == std::string::npos ? std::string() : str_tolower(output.substr(dot + 1));
    std::ofstream os(filename, std::ios::binary | std::ios::trunc);
    if (!os)
        throw std::runtime_error("Unable to create \"" + filename + "\"!");
    if (ext == "usdc" || ext == "usdz") {
        std::map<std::string, Value> meta;
        Prim root;
        root.path = "/";
        parse_usda(usdaText, output, meta, root);
        std::vector<uint8_t> data = encode_crate(meta, root);
        if (ext == "usdz") {
            std::string name = output.substr(output.find_last_of("/\\") == std::string::npos ? 0
                                             : output.find_last_of("/\\") + 1);
            name = name.substr(0, name.rfind('.')) + ".usdc";
            data = encode_usdz({ std::make_pair(name, std::move(data)) });
        }
        os.write((const char *) data.data(), (std::streamsize) data.size());
    } else {
        os.write(usdaText.data(), (std::streamsize) usdaText.size());
    }
    os.flush();
    if (!os)
        throw std::runtime_error("Error while writing \"" + filename + "\" (disk full?)!");
}

} // namespace usd
