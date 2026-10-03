/*
    objscene.cpp: Per-object access to Wavefront OBJ files (see objscene.h)
*/

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "objscene.h"
#include "meshio.h"
#include <fstream>
#include <map>
#include <set>
#include <cstdio>
#include <cstring>

namespace objscene {

namespace {

enum Kind : uint8_t { Other, Position, TexCoord, Normal, Element, Header, Material, Smooth };

const uint32_t NONE = 0xffffffffu;           /* absent vt / vn */
const uint32_t JOINED = 0xffffffffu;         /* line length: see Doc::joined */
const size_t NO_ELEM = (size_t) -1;

inline bool blank(char c) { return c == ' ' || c == '\t'; }

/* Bounds of the trimmed text [b, e) (spaces, tabs and '\r') */
inline void trim(const char *&b, const char *&e) {
    while (b < e && blank(*b))
        ++b;
    while (e > b && (blank(e[-1]) || e[-1] == '\r'))
        --e;
}

/* First word and the rest of a line, both trimmed */
void split_keyword(const char *b, const char *e, const char *&kb, const char *&ke,
                   const char *&rb, const char *&re) {
    trim(b, e);
    kb = b;
    while (b < e && !blank(*b))
        ++b;
    ke = b;
    rb = b;
    re = e;
    trim(rb, re);
}

void split_keyword(const std::string &line, std::string &keyword, std::string &rest) {
    const char *kb, *ke, *rb, *re;
    split_keyword(line.data(), line.data() + line.size(), kb, ke, rb, re);
    keyword.assign(kb, ke);
    rest.assign(rb, re);
}

inline bool is(const char *b, const char *e, const char *word) {
    const size_t n = strlen(word);
    return (size_t) (e - b) == n && memcmp(b, word, n) == 0;
}

void replace_file(const std::string &temp, const std::string &target) {
#if defined(_WIN32)
    const bool moved = MoveFileExA(temp.c_str(), target.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = std::rename(temp.c_str(), target.c_str()) == 0;
#endif
    if (!moved) {
        std::remove(temp.c_str());
        throw std::runtime_error("Unable to replace \"" + target + "\" (is it open in another program?)!");
    }
}

} // namespace

/* ------------------------------------------------------------------------- */
/*  Parsed file                                                              */
/* ------------------------------------------------------------------------- */

struct Scene::Doc {
    std::string filename, eol = "\n";
    std::string text;                        /* the whole file */

    /* Logical lines (continuations joined), as ranges of 'text' */
    std::vector<uint64_t> lineStart;
    std::vector<uint32_t> lineLength;        /* JOINED: the line is in 'joined' */
    std::map<size_t, std::string> joined;
    std::vector<uint8_t> kinds;
    std::vector<int32_t> headerObject;       /* object of each Header line, in order */

    /* Elements ("f", "l", "p"), in file order; corners in flat arrays */
    std::vector<char> elemType;
    std::vector<int32_t> elemObject;
    std::vector<uint32_t> elemMaterial, elemSmooth;   /* ids in 'strings' */
    std::vector<uint64_t> elemFirst;         /* first corner; one more entry at the end */
    std::vector<uint32_t> cornerV, cornerT, cornerN;  /* 0-based; NONE if absent */

    std::vector<Vector3f> positions;
    std::vector<float> uv;                   /* u, v of every "vt" line */
    size_t texcoords = 0, normals = 0;
    std::vector<std::string> names;
    std::map<std::string, int> ids;
    std::vector<std::string> strings { "" }; /* material and smoothing values */
    std::map<std::string, uint32_t> stringIds { { "", 0 } };

    size_t elems() const { return elemType.size(); }

    [[noreturn]] void fail(size_t line, const std::string &msg) const {
        throw std::runtime_error("OBJ file \"" + filename + "\", line " + std::to_string(line + 1) +
                                 ": " + msg + "!");
    }

    void line(size_t i, const char *&b, const char *&e) const {
        if (lineLength[i] == JOINED) {
            const std::string &s = joined.at(i);
            b = s.data();
            e = b + s.size();
        } else {
            b = text.data() + lineStart[i];
            e = b + lineLength[i];
        }
    }

    std::string line(size_t i) const {
        const char *b, *e;
        line(i, b, e);
        return std::string(b, e);
    }

    void write_line(std::ostream &os, size_t i) const {
        const char *b, *e;
        line(i, b, e);
        os.write(b, (std::streamsize) (e - b));
        os << eol;
    }

    int object(const std::string &name) {
        auto it = ids.find(name);
        if (it != ids.end())
            return it->second;
        const int id = (int) names.size();
        names.push_back(name);
        ids[name] = id;
        return id;
    }

    uint32_t intern(const char *b, const char *e) {
        std::string s(b, e);
        auto it = stringIds.find(s);
        if (it != stringIds.end())
            return it->second;
        const uint32_t id = (uint32_t) strings.size();
        strings.push_back(s);
        stringIds[s] = id;
        return id;
    }

    /* Replays the lines with the object each one belongs to (the last
       object line, or "default" from the first element before any) */
    template <typename Fn> void each_line(Fn fn) const {
        int current = -1;
        size_t h = 0, e = 0;
        for (size_t i = 0; i < kinds.size(); ++i) {
            const Kind kind = (Kind) kinds[i];
            size_t elem = NO_ELEM;
            if (kind == Header) {
                current = headerObject[h++];
            } else if (kind == Element) {
                elem = e++;
                current = elemObject[elem];
            }
            fn(i, kind, current, elem);
        }
    }

    size_t line_of_element(size_t k) const {
        size_t e = 0;
        for (size_t i = 0; i < kinds.size(); ++i)
            if (kinds[i] == Element && e++ == k)
                return i;
        return 0;
    }

    int find_object(const std::string &name) const {
        auto it = ids.find(name);
        if (it == ids.end())
            throw std::runtime_error("OBJ file \"" + filename + "\": no object named \"" + name + "\"!");
        for (size_t k = 0; k < elems(); ++k)
            if (elemObject[k] == it->second && elemType[k] == 'f')
                return it->second;
        throw std::runtime_error("OBJ file \"" + filename + "\": object \"" + name + "\" has no polygons!");
    }

    void parse();
    uint32_t resolve(size_t line, const char *b, const char *e, size_t count) const;
};

/* An OBJ index (1-based, or negative = relative) to 0-based; out of range
   absolute indices are caught after the whole file is read */
uint32_t Scene::Doc::resolve(size_t line, const char *b, const char *e, size_t count) const {
    const std::string token(b, e);
    if (token.empty())
        fail(line, "empty index");
    char *end = nullptr;
    const long long value = strtoll(token.c_str(), &end, 10);
    if (*end != '\0' || value == 0)
        fail(line, "invalid index \"" + token + "\"");
    if (value < 0) {
        if ((unsigned long long) (-value) > count)
            fail(line, "relative index " + token + " out of range");
        return (uint32_t) ((long long) count + value);
    }
    return (uint32_t) std::min<long long>(value - 1, (long long) NONE - 1);
}

void Scene::Doc::parse() {
    std::ifstream file(filename, std::ios::binary);
    if (!file)
        throw std::runtime_error("Unable to open OBJ file \"" + filename + "\"!");
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    if (size > 0) {
        text.resize((size_t) size);
        file.read(&text[0], size);
        if (!file)
            throw std::runtime_error("Unable to read OBJ file \"" + filename + "\"!");
    }
    if (text.find("\r\n") != std::string::npos)
        eol = "\r\n";

    /* Logical lines: a trailing backslash continues the line */
    {
        size_t pos = 0, pendingStart = 0;
        std::string pending;
        bool continued = false;
        while (pos < text.size()) {
            const char *nl = (const char *) memchr(text.data() + pos, '\n', text.size() - pos);
            size_t end = nl ? (size_t) (nl - text.data()) : text.size();
            const size_t next = end + 1;
            if (end > pos && text[end - 1] == '\r')
                --end;
            if (end > pos && text[end - 1] == '\\') {
                if (!continued)
                    pendingStart = pos;
                pending.append(text, pos, end - 1 - pos);
                pending += ' ';
                continued = true;
                pos = next;
                continue;
            }
            if (continued) {
                pending.append(text, pos, end - pos);
                joined[lineStart.size()] = pending;
                lineStart.push_back(pendingStart);
                lineLength.push_back(JOINED);
                pending.clear();
                continued = false;
            } else {
                if (end - pos >= JOINED)
                    throw std::runtime_error("OBJ file \"" + filename + "\": line too long!");
                lineStart.push_back(pos);
                lineLength.push_back((uint32_t) (end - pos));
            }
            pos = next;
        }
        if (continued) {
            joined[lineStart.size()] = pending;
            lineStart.push_back(pendingStart);
            lineLength.push_back(JOINED);
        }
    }
    const size_t n = lineStart.size();

    /* Objects are "o" blocks, or "g" groups when there is no "o" line */
    bool hasO = false;
    for (size_t i = 0; i < n && !hasO; ++i) {
        const char *b, *e, *kb, *ke, *rb, *re;
        line(i, b, e);
        split_keyword(b, e, kb, ke, rb, re);
        hasO = is(kb, ke, "o");
    }
    const char *objectKeyword = hasO ? "o" : "g";

    int current = -1;
    uint32_t material = 0, smooth = 0;
    kinds.assign(n, Other);
    std::string buffer;
    for (size_t i = 0; i < n; ++i) {
        const char *b, *e, *kb, *ke, *rb, *re;
        line(i, b, e);
        split_keyword(b, e, kb, ke, rb, re);
        if (kb == ke)
            continue;

        if (is(kb, ke, objectKeyword)) {
            current = object(rb == re ? std::string("default") : std::string(rb, re));
            headerObject.push_back(current);
            kinds[i] = Header;
        } else if (is(kb, ke, "v")) {
            buffer.assign(rb, re);
            Vector3f p;
            char *end = nullptr;
            const char *s = buffer.c_str();
            for (int k = 0; k < 3; ++k) {
                p[k] = (Float) strtod(s, &end);
                if (end == s)
                    fail(i, "invalid vertex position");
                s = end;
            }
            positions.push_back(p);
            kinds[i] = Position;
        } else if (is(kb, ke, "vt")) {
            buffer.assign(rb, re);
            char *end = nullptr;
            const char *s = buffer.c_str();
            const float u = (float) strtod(s, &end);
            const float v = (float) strtod(end, nullptr);
            uv.push_back(u);
            uv.push_back(v);
            ++texcoords;
            kinds[i] = TexCoord;
        } else if (is(kb, ke, "vn")) {
            ++normals;
            kinds[i] = Normal;
        } else if (ke - kb == 1 && (*kb == 'f' || *kb == 'l' || *kb == 'p')) {
            if (current < 0)
                current = object("default");
            const char type = *kb;
            const uint64_t first = cornerV.size();
            const char *body = rb, *bodyEnd = re;
            const char *hash = (const char *) memchr(body, '#', (size_t) (bodyEnd - body));
            if (hash)
                bodyEnd = hash;
            const char *t = body;
            while (t < bodyEnd) {
                while (t < bodyEnd && blank(*t))
                    ++t;
                const char *tokenEnd = t;
                while (tokenEnd < bodyEnd && !blank(*tokenEnd))
                    ++tokenEnd;
                if (tokenEnd == t)
                    break;
                /* v, v/vt, v//vn or v/vt/vn */
                const char *parts[3][2];
                int count = 0;
                const char *p = t;
                while (true) {
                    const char *slash = (const char *) memchr(p, '/', (size_t) (tokenEnd - p));
                    const char *partEnd = slash ? slash : tokenEnd;
                    if (count == 3)
                        fail(i, "invalid vertex data \"" + std::string(t, tokenEnd) + "\"");
                    parts[count][0] = p;
                    parts[count][1] = partEnd;
                    ++count;
                    if (!slash)
                        break;
                    p = slash + 1;
                }
                cornerV.push_back(resolve(i, parts[0][0], parts[0][1], positions.size()));
                cornerT.push_back(count >= 2 && parts[1][0] != parts[1][1]
                                  ? resolve(i, parts[1][0], parts[1][1], texcoords) : NONE);
                cornerN.push_back(count == 3 && parts[2][0] != parts[2][1]
                                  ? resolve(i, parts[2][0], parts[2][1], normals) : NONE);
                t = tokenEnd;
            }
            const size_t corners = (size_t) (cornerV.size() - first);
            const size_t minimum = type == 'f' ? 3 : (type == 'l' ? 2 : 1);
            if (corners < minimum)
                fail(i, std::string("\"") + type + "\" element with too few vertices");
            elemType.push_back(type);
            elemObject.push_back(current);
            elemMaterial.push_back(material);
            elemSmooth.push_back(smooth);
            elemFirst.push_back(first);
            kinds[i] = Element;
        } else if (is(kb, ke, "usemtl")) {
            material = intern(rb, re);
            kinds[i] = Material;
        } else if (is(kb, ke, "s")) {
            smooth = intern(rb, re);
            kinds[i] = Smooth;
        }
    }
    elemFirst.push_back(cornerV.size());

    /* Absolute indices may refer to data defined later: check at the end */
    for (size_t k = 0; k < elems(); ++k) {
        for (uint64_t c = elemFirst[k]; c < elemFirst[k + 1]; ++c) {
            if (cornerV[c] >= positions.size() ||
                (cornerT[c] != NONE && cornerT[c] >= texcoords) ||
                (cornerN[c] != NONE && cornerN[c] >= normals))
                fail(line_of_element(k), "index out of range");
        }
    }
}

/* ------------------------------------------------------------------------- */
/*  Scene                                                                    */
/* ------------------------------------------------------------------------- */

Scene::Scene(const std::string &filename) : d(new Doc()) {
    d->filename = filename;
    d->parse();
}

Scene::~Scene() { }

std::vector<ObjectInfo> Scene::objects() const {
    const size_t nObjects = d->names.size();
    std::vector<ObjectInfo> result(nObjects);
    std::vector<std::vector<uint32_t>> elemsOf(nObjects);
    for (size_t i = 0; i < nObjects; ++i)
        result[i].name = d->names[i];
    for (size_t k = 0; k < d->elems(); ++k) {
        if (d->elemType[k] != 'f')
            continue;
        result[d->elemObject[k]].faces++;
        elemsOf[d->elemObject[k]].push_back((uint32_t) k);
    }
    /* Distinct positions per object: one stamp per position */
    std::vector<int32_t> stamp(d->positions.size(), -1);
    std::vector<ObjectInfo> meshes;
    for (size_t i = 0; i < nObjects; ++i) {
        if (result[i].faces == 0)
            continue;
        for (uint32_t k : elemsOf[i])
            for (uint64_t c = d->elemFirst[k]; c < d->elemFirst[k + 1]; ++c)
                if (stamp[d->cornerV[c]] != (int32_t) i) {
                    stamp[d->cornerV[c]] = (int32_t) i;
                    result[i].vertices++;
                }
        meshes.push_back(result[i]);
    }
    return meshes;
}

void Scene::load(const std::string &name, MatrixXu &F, MatrixXf &V, uint64_t *polygons,
                 std::vector<UVSet> *uvs) const {
    const int id = d->find_object(name);
    std::vector<uint32_t> sizes, indices, texcoords;
    bool withoutUV = false;
    for (size_t k = 0; k < d->elems(); ++k) {
        if (d->elemObject[k] != id || d->elemType[k] != 'f')
            continue;
        sizes.push_back((uint32_t) (d->elemFirst[k + 1] - d->elemFirst[k]));
        for (uint64_t c = d->elemFirst[k]; c < d->elemFirst[k + 1]; ++c) {
            indices.push_back(d->cornerV[c]);
            texcoords.push_back(d->cornerT[c]);
            withoutUV |= d->cornerT[c] == NONE;
        }
    }

    /* One UV set when every corner of the object has a texture coordinate;
       only the values it uses are kept */
    if (uvs) {
        uvs->clear();
        if (!withoutUV && !texcoords.empty()) {
            std::vector<uint32_t> remap(d->texcoords, NONE);
            UVSet set;
            set.name = "uv";
            std::vector<float> values;
            for (uint32_t &t : texcoords) {
                if (remap[t] == NONE) {
                    remap[t] = (uint32_t) (values.size() / 2);
                    values.push_back(d->uv[2 * (size_t) t]);
                    values.push_back(d->uv[2 * (size_t) t + 1]);
                }
                t = remap[t];
            }
            set.values = Eigen::Map<const MatrixXf>(values.data(), 2, (std::ptrdiff_t) (values.size() / 2));
            set.corners.swap(texcoords);
            uvs->push_back(std::move(set));
        } else if (withoutUV && d->texcoords > 0) {
            cout << "Warning: some faces of \"" << name << "\" have no texture coordinates, UVs ignored" << endl;
        }
    }
    build_mesh(d->positions, sizes, indices, F, V, d->filename + ":" + name, uvs);
    if (polygons)
        *polygons = sizes.size();
}

namespace {

/* New geometry of a replaced object: polygons (counter-clockwise, as OBJ),
   only the vertices they use */
struct TargetGeometry {
    std::vector<Vector3f> positions;
    std::vector<uint32_t> sizes, indices;
    std::vector<float> uv;                /* texture coordinates (first UV set) */
    std::vector<uint32_t> uvIndices;      /* per corner, like 'indices' */
};

void target_geometry(const Replacement &r, TargetGeometry &t) {
    MatrixXu fetchedF;
    MatrixXf fetchedV;
    std::vector<CornerUVs> fetchedUVs;
    const MatrixXu *F = &r.F;
    const MatrixXf *V = &r.V;
    const std::vector<CornerUVs> *uvs = &r.uvs;
    if (r.fetch) {
        r.fetch(fetchedF, fetchedV, fetchedUVs);
        F = &fetchedF;
        V = &fetchedV;
        uvs = &fetchedUVs;
    }
    std::vector<uint32_t> faceIds, ccw, cornerIds;
    extracted_polygons(*F, t.sizes, ccw, faceIds, &cornerIds);
    if (!uvs->empty())
        indexed_uvs((*uvs)[0], cornerIds, t.uv, t.uvIndices);
    if (t.sizes.empty())
        throw std::runtime_error("OBJ writer: the new mesh of \"" + r.name + "\" is empty!");
    std::vector<int64_t> remap((size_t) V->cols(), -1);
    for (uint32_t index : ccw) {
        if (index >= V->cols())
            throw std::runtime_error("OBJ writer: vertex index out of range!");
        remap[index] = 0;
    }
    for (uint32_t i = 0; i < (uint32_t) V->cols(); ++i) {
        if (remap[i] < 0)
            continue;
        if (!V->col(i).allFinite())
            throw std::runtime_error("OBJ writer: invalid vertex position!");
        remap[i] = (int64_t) t.positions.size();
        t.positions.push_back(V->col(i));
    }
    t.indices.reserve(ccw.size());
    for (uint32_t index : ccw)
        t.indices.push_back((uint32_t) remap[index]);
}

} // namespace

void Scene::splice(const std::string &output, const std::vector<Replacement> &replacements) const {
    Timer<> timer;
    cout << "Writing \"" << output << "\" (" << replacements.size() << " object"
         << (replacements.size() > 1 ? "s" : "") << " replaced) .. ";
    cout.flush();

    const Doc &doc = *d;
    const size_t nObjects = doc.names.size();

    /* Targets, checked and counted first: their geometry is built again
       when written, one at a time */
    std::vector<const Replacement *> targets(nObjects, nullptr);
    std::vector<int64_t> targetVertices(nObjects, 0), targetTexcoords(nObjects, 0);
    std::vector<std::string> materialOf(nObjects);
    std::vector<std::string> notes;
    for (const Replacement &r : replacements) {
        const int id = doc.find_object(r.name);
        if (targets[id])
            throw std::runtime_error("Object \"" + r.name + "\" is replaced twice!");
        targets[id] = &r;
        TargetGeometry t;
        target_geometry(r, t);
        targetVertices[id] = (int64_t) t.positions.size();
        targetTexcoords[id] = (int64_t) (t.uv.size() / 2);
    }

    /* What the targets used, and what the other objects still need */
    std::vector<char> vT(doc.positions.size(), 0), vK(doc.positions.size(), 0);
    std::vector<char> tT(doc.texcoords, 0), tK(doc.texcoords, 0);
    std::vector<char> nT(doc.normals, 0), nK(doc.normals, 0);
    std::vector<std::map<uint32_t, size_t>> materials(nObjects);   /* faces per material */
    std::vector<std::vector<uint32_t>> materialOrder(nObjects);
    std::vector<char> hadUvs(nObjects, 0);
    for (size_t k = 0; k < doc.elems(); ++k) {
        const int obj = doc.elemObject[k];
        const bool target = targets[obj] != nullptr;
        for (uint64_t c = doc.elemFirst[k]; c < doc.elemFirst[k + 1]; ++c) {
            (target ? vT : vK)[doc.cornerV[c]] = 1;
            if (doc.cornerT[c] != NONE) {
                (target ? tT : tK)[doc.cornerT[c]] = 1;
                if (target)
                    hadUvs[obj] = 1;
            }
            if (doc.cornerN[c] != NONE) {
                (target ? nT : nK)[doc.cornerN[c]] = 1;
                if (target)
                    hadUvs[obj] = 1;
            }
        }
        if (target && doc.elemType[k] == 'f') {
            if (materials[obj][doc.elemMaterial[k]]++ == 0)
                materialOrder[obj].push_back(doc.elemMaterial[k]);
        }
    }
    for (size_t id = 0; id < nObjects; ++id) {
        if (!targets[id])
            continue;
        /* OBJ faces inherit the last "usemtl": an object cannot be left
           without material, so keep its most used one (the first in file
           order on a tie) rather than inheriting another object's */
        uint32_t best = 0;
        size_t bestCount = 0;
        for (uint32_t m : materialOrder[id]) {
            if (materials[id][m] > bestCount) {
                best = m;
                bestCount = materials[id][m];
            }
        }
        materialOf[id] = doc.strings[best];
        if (materials[id].size() > 1)
            notes.push_back(doc.names[id] + ": kept material \"" + materialOf[id] + "\" (its most used), dropped " +
                            std::to_string(materials[id].size() - 1) +
                            " other(s): per-face materials cannot follow the new faces");
        if (hadUvs[id])
            notes.push_back(doc.names[id] + (targetTexcoords[id] > 0
                ? ": new UVs written, normals dropped (no longer matching the topology)"
                : ": dropped UVs and normals (no longer matching the topology)"));
    }
    auto keep = [](const std::vector<char> &target, const std::vector<char> &kept, size_t i) {
        return !target[i] || kept[i];
    };

    /* Pass 1: new numbering, in output order */
    std::vector<int64_t> firstLine(nObjects, -1);
    doc.each_line([&](size_t i, Kind, int obj, size_t) {
        if (obj >= 0 && firstLine[obj] < 0)
            firstLine[obj] = (int64_t) i;
    });

    std::vector<int64_t> newV(doc.positions.size(), -1), newT(doc.texcoords, -1), newN(doc.normals, -1);
    std::vector<int64_t> base(nObjects, 0), baseT(nObjects, 0);
    {
        int64_t cv = 0, ct = 0, cn = 0;
        size_t kv = 0, kt = 0, kn = 0;
        doc.each_line([&](size_t i, Kind kind, int obj, size_t) {
            if (obj >= 0 && targets[obj] && firstLine[obj] == (int64_t) i) {
                base[obj] = cv;
                cv += targetVertices[obj];
                baseT[obj] = ct;
                ct += targetTexcoords[obj];
            }
            if (kind == Position) {
                if (keep(vT, vK, kv))
                    newV[kv] = cv++;
                ++kv;
            } else if (kind == TexCoord) {
                if (keep(tT, tK, kt))
                    newT[kt] = ct++;
                ++kt;
            } else if (kind == Normal) {
                if (keep(nT, nK, kn))
                    newN[kn] = cn++;
                ++kn;
            }
        });
    }

    /* Pass 2: write (into a temporary file, removed on any error) */
    const std::string temp = output + ".tmp";
    struct TempGuard {
        const std::string &path;
        bool armed = true;
        ~TempGuard() { if (armed) std::remove(path.c_str()); }
    } guard { temp };
    {
        std::ofstream os(temp, std::ios::binary | std::ios::trunc);
        if (!os)
            throw std::runtime_error("Unable to create \"" + temp + "\"!");
        const std::string &eol = doc.eol;
        uint32_t outMaterial = 0, outSmooth = 0;
        std::string keyword, rest;
        size_t kv = 0, kt = 0, kn = 0;
        char buf[128];
        doc.each_line([&](size_t i, Kind kind, int obj, size_t elem) {
            const bool target = obj >= 0 && targets[obj] != nullptr;

            if (target && firstLine[obj] == (int64_t) i) {
                if (kind == Header)
                    doc.write_line(os, i);
                if (!materialOf[obj].empty()) {
                    os << "usemtl " << materialOf[obj] << eol;
                    outMaterial = doc.stringIds.at(materialOf[obj]);
                }
                TargetGeometry t;
                target_geometry(*targets[obj], t);
                for (const Vector3f &p : t.positions) {
                    snprintf(buf, sizeof(buf), "v %.9g %.9g %.9g", p.x(), p.y(), p.z());
                    os << buf << eol;
                }
                for (size_t k = 0; k < t.uv.size(); k += 2) {
                    snprintf(buf, sizeof(buf), "vt %.9g %.9g", t.uv[k], t.uv[k + 1]);
                    os << buf << eol;
                }
                const bool withUVs = !t.uvIndices.empty();
                size_t offset = 0;
                for (uint32_t size : t.sizes) {
                    os << "f";
                    for (uint32_t k = 0; k < size; ++k) {
                        os << " " << (base[obj] + t.indices[offset + k] + 1);
                        if (withUVs)
                            os << "/" << (baseT[obj] + t.uvIndices[offset + k] + 1);
                    }
                    os << eol;
                    offset += size;
                }
                if (kind == Header)
                    return;
            }

            if (kind == Position || kind == TexCoord || kind == Normal) {
                size_t &k = kind == Position ? kv : (kind == TexCoord ? kt : kn);
                const std::vector<int64_t> &map = kind == Position ? newV : (kind == TexCoord ? newT : newN);
                if (map[k] >= 0)
                    doc.write_line(os, i);
                ++k;
                return;
            }
            if (target)
                return;   /* the rest of a replaced object: faces, materials, groups, comments */

            if (kind == Element) {
                /* Keep the inherited material / smoothing state even if a
                   replaced object used to set it */
                const uint32_t material = doc.elemMaterial[elem], smooth = doc.elemSmooth[elem];
                if (material != outMaterial && material != 0) {
                    os << "usemtl " << doc.strings[material] << eol;
                    outMaterial = material;
                }
                if (smooth != outSmooth && smooth != 0) {
                    os << "s " << doc.strings[smooth] << eol;
                    outSmooth = smooth;
                }
                os << doc.elemType[elem];
                for (uint64_t c = doc.elemFirst[elem]; c < doc.elemFirst[elem + 1]; ++c) {
                    const int64_t v = newV[doc.cornerV[c]];
                    if (v < 0)
                        throw std::runtime_error("OBJ writer: internal error (dropped vertex still used)!");
                    os << " " << (v + 1);
                    const bool hasT = doc.cornerT[c] != NONE, hasN = doc.cornerN[c] != NONE;
                    if (hasT || hasN) {
                        os << "/";
                        if (hasT)
                            os << (newT[doc.cornerT[c]] + 1);
                        if (hasN)
                            os << "/" << (newN[doc.cornerN[c]] + 1);
                    }
                }
                os << eol;
            } else {
                if (kind == Material || kind == Smooth) {
                    split_keyword(doc.line(i), keyword, rest);
                    auto it = doc.stringIds.find(rest);
                    (kind == Material ? outMaterial : outSmooth) = it->second;
                }
                doc.write_line(os, i);
            }
        });
        os.flush();
        if (!os)
            throw std::runtime_error("Error while writing \"" + temp + "\" (disk full?)!");
    }
    replace_file(temp, output);
    guard.armed = false;

    cout << "done. (took " << timeString(timer.value()) << ")" << endl;
    for (const std::string &note : notes)
        cout << "   " << note << endl;
}

/* ------------------------------------------------------------------------- */
/*  Shortcuts                                                                */
/* ------------------------------------------------------------------------- */

std::vector<ObjectInfo> list_objects(const std::string &filename) {
    return Scene(filename).objects();
}

void load_object(const std::string &filename, const std::string &name,
                 MatrixXu &F, MatrixXf &V, uint64_t *polygons, std::vector<UVSet> *uvs) {
    Scene(filename).load(name, F, V, polygons, uvs);
}

void splice_obj(const std::string &input, const std::string &output,
                const std::vector<Replacement> &replacements) {
    Scene(input).splice(output, replacements);
}

} // namespace objscene
