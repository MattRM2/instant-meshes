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

namespace objscene {

namespace {

enum Kind { Other, Position, TexCoord, Normal, Element, Header, Material, Smooth };

struct Corner {
    int64_t v = -1, vt = -1, vn = -1;   /* 0-based absolute indices */
    bool hasVt = false, hasVn = false;
};

struct Elem {
    char type;                           /* 'f', 'l' or 'p' */
    std::vector<Corner> corners;
    std::string material, smooth;        /* state in effect for this element */
    int object;
};

struct Doc {
    std::string filename, eol = "\n";
    std::vector<std::string> lines;      /* logical lines (continuations joined) */
    std::vector<Kind> kinds;
    std::vector<int> lineObject;         /* object of each line, -1 before any */
    std::vector<int> lineElem;           /* element index of Element lines */
    std::vector<Elem> elems;
    std::vector<Vector3f> positions;
    size_t texcoords = 0, normals = 0;
    std::vector<std::string> names;
    std::map<std::string, int> ids;

    [[noreturn]] void fail(size_t line, const std::string &msg) const {
        throw std::runtime_error("OBJ file \"" + filename + "\", line " + std::to_string(line + 1) +
                                 ": " + msg + "!");
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
};

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t'))
        ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r'))
        --b;
    return s.substr(a, b - a);
}

/* First word and the rest of a line */
void split_keyword(const std::string &line, std::string &keyword, std::string &rest) {
    const std::string t = trim(line);
    size_t i = 0;
    while (i < t.size() && t[i] != ' ' && t[i] != '\t')
        ++i;
    keyword = t.substr(0, i);
    rest = trim(t.substr(i));
}

/* Resolves an OBJ index (1-based, or negative = relative) to 0-based */
int64_t resolve(const Doc &doc, size_t line, const std::string &token, size_t count) {
    if (token.empty())
        doc.fail(line, "empty index");
    char *end = nullptr;
    const long long value = strtoll(token.c_str(), &end, 10);
    if (*end != '\0' || value == 0)
        doc.fail(line, "invalid index \"" + token + "\"");
    if (value < 0) {
        if ((unsigned long long) (-value) > count)
            doc.fail(line, "relative index " + token + " out of range");
        return (int64_t) count + value;
    }
    return value - 1;
}

Doc parse(const std::string &filename) {
    Doc doc;
    doc.filename = filename;
    std::ifstream is(filename, std::ios::binary);
    if (!is)
        throw std::runtime_error("Unable to open OBJ file \"" + filename + "\"!");
    std::string text((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    if (text.find("\r\n") != std::string::npos)
        doc.eol = "\r\n";

    /* Logical lines: a trailing backslash continues the line */
    size_t pos = 0;
    std::string pending;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        pos = end + 1;
        if (!line.empty() && line.back() == '\\') {
            line.pop_back();
            pending += line + " ";
            continue;
        }
        doc.lines.push_back(pending + line);
        pending.clear();
    }
    if (!pending.empty())
        doc.lines.push_back(pending);

    /* Objects are "o" blocks, or "g" groups when there is no "o" line */
    bool hasO = false;
    for (const std::string &l : doc.lines) {
        std::string k, r;
        split_keyword(l, k, r);
        if (k == "o") {
            hasO = true;
            break;
        }
    }
    const std::string objectKeyword = hasO ? "o" : "g";

    int current = -1;
    std::string material, smooth;
    const size_t n = doc.lines.size();
    doc.kinds.assign(n, Other);
    doc.lineObject.assign(n, -1);
    doc.lineElem.assign(n, -1);

    for (size_t i = 0; i < n; ++i) {
        std::string keyword, rest;
        split_keyword(doc.lines[i], keyword, rest);

        if (keyword == objectKeyword) {
            current = doc.object(rest.empty() ? "default" : rest);
            doc.kinds[i] = Header;
        } else if (keyword == "v") {
            Vector3f p;
            char *end = nullptr;
            const char *s = rest.c_str();
            for (int k = 0; k < 3; ++k) {
                p[k] = (Float) strtod(s, &end);
                if (end == s)
                    doc.fail(i, "invalid vertex position");
                s = end;
            }
            doc.positions.push_back(p);
            doc.kinds[i] = Position;
        } else if (keyword == "vt") {
            ++doc.texcoords;
            doc.kinds[i] = TexCoord;
        } else if (keyword == "vn") {
            ++doc.normals;
            doc.kinds[i] = Normal;
        } else if (keyword == "f" || keyword == "l" || keyword == "p") {
            if (current < 0)
                current = doc.object("default");
            Elem e;
            e.type = keyword[0];
            e.material = material;
            e.smooth = smooth;
            e.object = current;
            const std::string body = rest.substr(0, rest.find('#'));
            std::vector<std::string> tokens = str_tokenize(body, ' ', false);
            for (const std::string &token : tokens) {
                if (trim(token).empty())
                    continue;
                std::vector<std::string> parts = str_tokenize(trim(token), '/', true);
                if (parts.empty() || parts.size() > 3)
                    doc.fail(i, "invalid vertex data \"" + token + "\"");
                Corner c;
                c.v = resolve(doc, i, parts[0], doc.positions.size());
                if (parts.size() >= 2 && !parts[1].empty()) {
                    c.vt = resolve(doc, i, parts[1], doc.texcoords);
                    c.hasVt = true;
                }
                if (parts.size() == 3 && !parts[2].empty()) {
                    c.vn = resolve(doc, i, parts[2], doc.normals);
                    c.hasVn = true;
                }
                e.corners.push_back(c);
            }
            const size_t minimum = e.type == 'f' ? 3 : (e.type == 'l' ? 2 : 1);
            if (e.corners.size() < minimum)
                doc.fail(i, std::string("\"") + e.type + "\" element with too few vertices");
            doc.kinds[i] = Element;
            doc.lineElem[i] = (int) doc.elems.size();
            doc.elems.push_back(e);
        } else if (keyword == "usemtl") {
            material = rest;
            doc.kinds[i] = Material;
        } else if (keyword == "s") {
            smooth = rest;
            doc.kinds[i] = Smooth;
        }
        doc.lineObject[i] = current;
    }

    /* Absolute indices may refer to data defined later: check at the end */
    for (size_t k = 0; k < doc.elems.size(); ++k) {
        for (const Corner &c : doc.elems[k].corners) {
            if (c.v < 0 || c.v >= (int64_t) doc.positions.size() ||
                (c.hasVt && (c.vt < 0 || c.vt >= (int64_t) doc.texcoords)) ||
                (c.hasVn && (c.vn < 0 || c.vn >= (int64_t) doc.normals))) {
                size_t line = 0;
                for (size_t i = 0; i < n; ++i)
                    if (doc.lineElem[i] == (int) k)
                        line = i;
                doc.fail(line, "index out of range");
            }
        }
    }
    return doc;
}

int find_object(const Doc &doc, const std::string &name) {
    auto it = doc.ids.find(name);
    if (it == doc.ids.end())
        throw std::runtime_error("OBJ file \"" + doc.filename + "\": no object named \"" + name + "\"!");
    for (const Elem &e : doc.elems)
        if (e.object == it->second && e.type == 'f')
            return it->second;
    throw std::runtime_error("OBJ file \"" + doc.filename + "\": object \"" + name + "\" has no polygons!");
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

std::vector<ObjectInfo> list_objects(const std::string &filename) {
    Doc doc = parse(filename);
    std::vector<ObjectInfo> result(doc.names.size());
    std::vector<std::set<int64_t>> used(doc.names.size());
    for (size_t i = 0; i < doc.names.size(); ++i)
        result[i].name = doc.names[i];
    for (const Elem &e : doc.elems) {
        if (e.type != 'f')
            continue;
        result[e.object].faces++;
        for (const Corner &c : e.corners)
            used[e.object].insert(c.v);
    }
    std::vector<ObjectInfo> meshes;
    for (size_t i = 0; i < result.size(); ++i) {
        if (result[i].faces == 0)
            continue;
        result[i].vertices = used[i].size();
        meshes.push_back(result[i]);
    }
    return meshes;
}

void load_object(const std::string &filename, const std::string &name,
                 MatrixXu &F, MatrixXf &V, uint64_t *polygons) {
    Doc doc = parse(filename);
    const int id = find_object(doc, name);
    std::vector<uint32_t> sizes, indices;
    for (const Elem &e : doc.elems) {
        if (e.object != id || e.type != 'f')
            continue;
        sizes.push_back((uint32_t) e.corners.size());
        for (const Corner &c : e.corners)
            indices.push_back((uint32_t) c.v);
    }
    build_mesh(doc.positions, sizes, indices, F, V, filename + ":" + name);
    if (polygons)
        *polygons = sizes.size();
}

void splice_obj(const std::string &input, const std::string &output,
                const std::vector<Replacement> &replacements) {
    Timer<> timer;
    cout << "Writing \"" << output << "\" (" << replacements.size() << " object"
         << (replacements.size() > 1 ? "s" : "") << " replaced) .. ";
    cout.flush();

    Doc doc = parse(input);
    const size_t nObjects = doc.names.size();

    /* New geometry of each target: polygons (counter-clockwise, as OBJ),
       only the vertices they use */
    struct Target {
        std::vector<Vector3f> positions;
        std::vector<uint32_t> sizes, indices;
        std::string material;
        bool active = false;
    };
    std::vector<Target> targets(nObjects);
    std::vector<std::string> notes;
    for (const Replacement &r : replacements) {
        const int id = find_object(doc, r.name);
        Target &t = targets[id];
        if (t.active)
            throw std::runtime_error("Object \"" + r.name + "\" is replaced twice!");
        t.active = true;
        std::vector<uint32_t> faceIds, ccw;
        extracted_polygons(r.F, t.sizes, ccw, faceIds);
        if (t.sizes.empty())
            throw std::runtime_error("OBJ writer: the new mesh of \"" + r.name + "\" is empty!");
        std::vector<int64_t> remap((size_t) r.V.cols(), -1);
        for (uint32_t index : ccw) {
            if (index >= r.V.cols())
                throw std::runtime_error("OBJ writer: vertex index out of range!");
            if (remap[index] < 0) {
                remap[index] = 0;
            }
        }
        for (uint32_t i = 0; i < (uint32_t) r.V.cols(); ++i) {
            if (remap[i] < 0)
                continue;
            if (!r.V.col(i).allFinite())
                throw std::runtime_error("OBJ writer: invalid vertex position!");
            remap[i] = (int64_t) t.positions.size();
            t.positions.push_back(r.V.col(i));
        }
        for (uint32_t index : ccw)
            t.indices.push_back((uint32_t) remap[index]);
    }

    /* What the targets used, and what the other objects still need */
    std::vector<char> vT(doc.positions.size(), 0), vK(doc.positions.size(), 0);
    std::vector<char> tT(doc.texcoords, 0), tK(doc.texcoords, 0);
    std::vector<char> nT(doc.normals, 0), nK(doc.normals, 0);
    std::vector<std::map<std::string, size_t>> materials(nObjects);   /* faces per material */
    std::vector<std::vector<std::string>> materialOrder(nObjects);
    std::vector<char> hadUvs(nObjects, 0);
    for (const Elem &e : doc.elems) {
        const bool target = targets[e.object].active;
        for (const Corner &c : e.corners) {
            (target ? vT : vK)[(size_t) c.v] = 1;
            if (c.hasVt) {
                (target ? tT : tK)[(size_t) c.vt] = 1;
                if (target)
                    hadUvs[e.object] = 1;
            }
            if (c.hasVn) {
                (target ? nT : nK)[(size_t) c.vn] = 1;
                if (target)
                    hadUvs[e.object] = 1;
            }
        }
        if (target && e.type == 'f') {
            if (materials[e.object][e.material]++ == 0)
                materialOrder[e.object].push_back(e.material);
        }
    }
    for (size_t id = 0; id < nObjects; ++id) {
        if (!targets[id].active)
            continue;
        /* OBJ faces inherit the last "usemtl": an object cannot be left
           without material, so keep its most used one (the first in file
           order on a tie) rather than inheriting another object's */
        std::string best;
        size_t bestCount = 0;
        for (const std::string &m : materialOrder[id]) {
            if (materials[id][m] > bestCount) {
                best = m;
                bestCount = materials[id][m];
            }
        }
        targets[id].material = best;
        if (materials[id].size() > 1)
            notes.push_back(doc.names[id] + ": kept material \"" + best + "\" (its most used), dropped " +
                            std::to_string(materials[id].size() - 1) +
                            " other(s): per-face materials cannot follow the new faces");
        if (hadUvs[id])
            notes.push_back(doc.names[id] + ": dropped UVs and normals (no longer matching the topology)");
    }
    auto keep = [](const std::vector<char> &target, const std::vector<char> &kept, size_t i) {
        return !target[i] || kept[i];
    };

    /* Pass 1: new numbering, in output order */
    const size_t n = doc.lines.size();
    std::vector<int> firstLine(nObjects, -1);
    for (size_t i = 0; i < n; ++i)
        if (doc.lineObject[i] >= 0 && firstLine[doc.lineObject[i]] < 0)
            firstLine[doc.lineObject[i]] = (int) i;

    std::vector<int64_t> newV(doc.positions.size(), -1), newT(doc.texcoords, -1), newN(doc.normals, -1);
    std::vector<int64_t> base(nObjects, 0);
    {
        int64_t cv = 0, ct = 0, cn = 0;
        size_t kv = 0, kt = 0, kn = 0;
        for (size_t i = 0; i < n; ++i) {
            const int obj = doc.lineObject[i];
            if (obj >= 0 && targets[obj].active && firstLine[obj] == (int) i) {
                base[obj] = cv;
                cv += (int64_t) targets[obj].positions.size();
            }
            if (doc.kinds[i] == Position) {
                if (keep(vT, vK, kv))
                    newV[kv] = cv++;
                ++kv;
            } else if (doc.kinds[i] == TexCoord) {
                if (keep(tT, tK, kt))
                    newT[kt] = ct++;
                ++kt;
            } else if (doc.kinds[i] == Normal) {
                if (keep(nT, nK, kn))
                    newN[kn] = cn++;
                ++kn;
            }
        }
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
        std::string outMaterial, outSmooth;
        size_t kv = 0, kt = 0, kn = 0;
        char buf[128];
        for (size_t i = 0; i < n; ++i) {
            const int obj = doc.lineObject[i];
            const bool target = obj >= 0 && targets[obj].active;
            const Kind kind = doc.kinds[i];

            if (target && firstLine[obj] == (int) i) {
                if (kind == Header)
                    os << doc.lines[i] << eol;
                const Target &t = targets[obj];
                if (!t.material.empty()) {
                    os << "usemtl " << t.material << eol;
                    outMaterial = t.material;
                }
                for (const Vector3f &p : t.positions) {
                    snprintf(buf, sizeof(buf), "v %.9g %.9g %.9g", p.x(), p.y(), p.z());
                    os << buf << eol;
                }
                size_t offset = 0;
                for (uint32_t size : t.sizes) {
                    os << "f";
                    for (uint32_t k = 0; k < size; ++k)
                        os << " " << (base[obj] + t.indices[offset + k] + 1);
                    os << eol;
                    offset += size;
                }
                if (kind == Header)
                    continue;
            }

            if (kind == Position || kind == TexCoord || kind == Normal) {
                size_t &k = kind == Position ? kv : (kind == TexCoord ? kt : kn);
                const std::vector<int64_t> &map = kind == Position ? newV : (kind == TexCoord ? newT : newN);
                if (map[k] >= 0)
                    os << doc.lines[i] << eol;
                ++k;
                continue;
            }
            if (target)
                continue;   /* the rest of a replaced object: faces, materials, groups, comments */

            if (kind == Element) {
                const Elem &e = doc.elems[(size_t) doc.lineElem[i]];
                /* Keep the inherited material / smoothing state even if a
                   replaced object used to set it */
                if (e.material != outMaterial && !e.material.empty()) {
                    os << "usemtl " << e.material << eol;
                    outMaterial = e.material;
                }
                if (e.smooth != outSmooth && !e.smooth.empty()) {
                    os << "s " << e.smooth << eol;
                    outSmooth = e.smooth;
                }
                os << e.type;
                for (const Corner &c : e.corners) {
                    const int64_t v = newV[(size_t) c.v];
                    if (v < 0)
                        throw std::runtime_error("OBJ writer: internal error (dropped vertex still used)!");
                    os << " " << (v + 1);
                    if (c.hasVt || c.hasVn) {
                        os << "/";
                        if (c.hasVt)
                            os << (newT[(size_t) c.vt] + 1);
                        if (c.hasVn)
                            os << "/" << (newN[(size_t) c.vn] + 1);
                    }
                }
                os << eol;
            } else {
                std::string keyword;
                if (kind == Material)
                    split_keyword(doc.lines[i], keyword, outMaterial);
                if (kind == Smooth)
                    split_keyword(doc.lines[i], keyword, outSmooth);
                os << doc.lines[i] << eol;
            }
        }
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

} // namespace objscene
