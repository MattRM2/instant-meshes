/*
    usdscene.cpp: meshes of a USD layer, and the .usda layers written (see usdscene.h)
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

#include "usdscene.h"
#include <Eigen/Geometry>
#include <fstream>
#include <functional>
#include <deque>
#include <set>
#include <unordered_map>
#include <cstdio>

namespace usd {

bool is_usd_file(const std::string &filename) {
    const size_t dot = filename.rfind('.');
    if (dot == std::string::npos)
        return false;
    const std::string ext = str_tolower(filename.substr(dot));
    return ext == ".usd" || ext == ".usda" || ext == ".usdc" || ext == ".usdz";
}

namespace {

typedef Eigen::Matrix4d Mat4;

[[noreturn]] void fail(const Layer &layer, const std::string &msg) {
    throw std::runtime_error("USD file \"" + layer.filename() + "\": " + msg + "!");
}

/* Default value, else the first time sample (the first frame, as for Alembic) */
Value first_value(const Layer &layer, const Property &p, bool *animated = nullptr) {
    Value v = layer.value(p);
    if (p.hasTimeSamples) {
        if (animated)
            *animated = true;
        if (v.kind == Value::Empty) {
            Value s = layer.samples(p);
            if (!s.samples.empty())
                return s.samples.front().second;
        }
    }
    return v;
}

Mat4 rotation(int axis, double degrees) {
    Mat4 m = Mat4::Identity();
    m.topLeftCorner<3, 3>() = Eigen::AngleAxisd(degrees * M_PI / 180.0, Eigen::Vector3d::Unit(axis)).toRotationMatrix();
    return m;
}

/* Local matrix of a prim (column vectors: p' = M p): xformOpOrder, op by
   op, outermost first; 'reset' if it starts with !resetXformStack! */
Mat4 local_transform(const Layer &layer, const Prim &prim, bool &reset, bool &animated) {
    Mat4 M = Mat4::Identity();
    reset = false;
    const Property *order = prim.property("xformOpOrder");
    if (!order)
        return M;
    const Value ops = first_value(layer, *order);
    for (const std::string &entry : ops.strings) {
        if (entry == "!resetXformStack!") {
            reset = true;
            M = Mat4::Identity();
            continue;
        }
        const bool invert = entry.compare(0, 8, "!invert!") == 0;
        const std::string name = invert ? entry.substr(8) : entry;
        const Property *op = prim.property(name);
        if (!op)
            fail(layer, "transform op \"" + name + "\" of \"" + prim.path + "\" is not authored");
        const Value v = first_value(layer, *op, &animated);
        const std::vector<double> &n = v.numbers;
        /* "xformOp:<type>[:suffix]" */
        const size_t a = name.find(':'), b = name.find(':', a + 1);
        const std::string type = name.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
        auto need = [&](size_t count) {
            if (n.size() < count)
                fail(layer, "transform op \"" + name + "\" of \"" + prim.path + "\" has no value");
        };
        Mat4 C = Mat4::Identity();
        if (type == "translate") {
            need(3);
            C.block<3, 1>(0, 3) = Eigen::Vector3d(n[0], n[1], n[2]);
        } else if (type == "translateX" || type == "translateY" || type == "translateZ") {
            need(1);
            C(type.back() - 'X', 3) = n[0];
        } else if (type == "scale") {
            need(3);
            C(0, 0) = n[0];
            C(1, 1) = n[1];
            C(2, 2) = n[2];
        } else if (type == "scaleX" || type == "scaleY" || type == "scaleZ") {
            need(1);
            const int k = type.back() - 'X';
            C(k, k) = n[0];
        } else if (type == "rotateX" || type == "rotateY" || type == "rotateZ") {
            need(1);
            C = rotation(type.back() - 'X', n[0]);
        } else if (type.size() == 9 && type.compare(0, 6, "rotate") == 0) {
            /* rotateXYZ: X first, then Y, then Z (angles in that order) */
            need(3);
            for (int k = 0; k < 3; ++k) {
                const int axis = type[6 + k] - 'X';
                if (axis < 0 || axis > 2)
                    fail(layer, "unknown transform op \"" + name + "\"");
                C = rotation(axis, n[(size_t) axis]) * C;
            }
        } else if (type == "orient") {
            need(4);
            Eigen::Quaterniond q(n[0], n[1], n[2], n[3]);
            if (q.norm() > 0)
                q.normalize();
            C.topLeftCorner<3, 3>() = q.toRotationMatrix();
        } else if (type == "transform") {
            need(16);
            /* row vectors in USD: the transpose */
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c)
                    C(c, r) = n[(size_t) (4 * r + c)];
        } else {
            fail(layer, "unknown transform op \"" + name + "\" of \"" + prim.path + "\"");
        }
        if (!C.allFinite())
            fail(layer, "transform op \"" + name + "\" of \"" + prim.path + "\" has invalid values");
        if (invert) {
            const double det = C.determinant();
            if (!(std::abs(det) > 1e-300))
                fail(layer, "transform op \"" + name + "\" of \"" + prim.path + "\" cannot be inverted");
            C = C.inverse().eval();
        }
        M = M * C;
    }
    return M;
}

bool active(const Prim &p) {
    const Value *a = p.metadata("active");
    return !(a && a->kind == Value::Numbers && a->num() == 0);
}

bool instanceable(const Prim &p) {
    const Value *a = p.metadata("instanceable");
    return a && a->kind == Value::Numbers && a->num() != 0;
}

/* Walks the defined, active prims with their world matrix */
struct Walker {
    const Layer &layer;
    std::function<void(const Prim &, const Mat4 &, bool instanced)> onMesh;
    std::function<void(const Prim &, const Mat4 &)> onPrim;
    size_t visits = 0;

    void run() {
        for (const auto &c : layer.root.children)
            visit(*c, Mat4::Identity(), false, 0);
    }

    void visit(const Prim &p, const Mat4 &parent, bool instanced, int depth) {
        if (p.specifier != Specifier::Def || !active(p))
            return;
        if (depth > 1000 || ++visits > 100000000)
            fail(layer, "prims nested too deeply or too many prims");
        bool reset, animated = false;
        const Mat4 local = local_transform(layer, p, reset, animated);
        const Mat4 world = reset ? local : Mat4(parent * local);
        const bool inst = instanced || instanceable(p) || p.type == "PointInstancer";
        if (onPrim)
            onPrim(p, world);
        if (p.type == "Mesh" && onMesh)
            onMesh(p, world, inst);
        for (const auto &c : p.children)
            visit(*c, world, inst, depth + 1);
    }
};

/* Texture coordinate primvars: texCoord2*, or a float2 / double2 / half2
   named like a UV set */
bool uv_primvar(const Property &p, std::string &name) {
    if (p.relationship || p.name.compare(0, 9, "primvars:") != 0)
        return false;
    name = p.name.substr(9);
    if (name.size() > 8 && name.compare(name.size() - 8, 8, ":indices") == 0)
        return false;
    if (p.type.compare(0, 9, "texCoord2") == 0)
        return true;
    if (p.type != "float2[]" && p.type != "double2[]" && p.type != "half2[]")
        return false;
    const std::string lower = str_tolower(name);
    return lower == "st" || lower == "uv" || lower == "uvmap" || lower == "map1" ||
           lower.compare(0, 2, "st") == 0 || lower.compare(0, 2, "uv") == 0;
}

std::string interpolation(const Property &p, const char *fallback) {
    auto it = p.meta.find("interpolation");
    return it == p.meta.end() ? std::string(fallback) : it->second.str();
}

/* Geometry of one mesh in world space: polygons counter-clockwise, UV sets
   per polygon corner, appended to the collected data */
struct Collector {
    const Layer &layer;
    bool wantUVs = false;
    std::vector<Vector3f> positions;
    std::vector<uint32_t> sizes, indices;
    uint64_t skippedFaces = 0;
    size_t meshes = 0;

    struct UVAccum {
        std::string name;
        std::vector<float> values;
        std::vector<uint32_t> corners;
        size_t meshes = 0;
    };
    std::deque<UVAccum> uvs;   /* deque: pointers to elements stay valid */

    void add(const Prim &p, const Mat4 &world) {
        const Property *pp = p.property("points"), *pc = p.property("faceVertexCounts"),
                       *pi = p.property("faceVertexIndices");
        if (!pp || !pc || !pi)
            fail(layer, "mesh \"" + p.path + "\" lacks points or faces");
        const Value points = first_value(layer, *pp), counts = first_value(layer, *pc),
                    corners = first_value(layer, *pi);
        if (points.tuple != 3 && !points.numbers.empty())
            fail(layer, "mesh \"" + p.path + "\" has points of an unsupported type");
        const size_t nPoints = points.numbers.size() / 3;
        uint64_t total = 0;
        for (double c : counts.numbers) {
            if (c < 0 || c > 1e9)
                fail(layer, "mesh \"" + p.path + "\" has an invalid face size");
            total += (uint64_t) c;
        }
        if (total != corners.numbers.size())
            fail(layer, "mesh \"" + p.path + "\": face counts do not match the face indices");
        for (double i : corners.numbers)
            if (i < 0 || i >= (double) nPoints)
                fail(layer, "mesh \"" + p.path + "\": vertex index " + std::to_string((long long) i) + " out of range");
        bool leftHanded = false;
        if (const Property *o = p.property("orientation"))
            leftHanded = first_value(layer, *o).str() == "leftHanded";

        const uint64_t base = positions.size();
        if (base + nPoints > 0x7fffffffULL || indices.size() + corners.numbers.size() > 0x7fffffffULL)
            fail(layer, "too much geometry (more than 2^31 vertices or face corners)");
        positions.reserve(positions.size() + nPoints);
        for (size_t k = 0; k < nPoints; ++k) {
            const Eigen::Vector4d q = world * Eigen::Vector4d(points.numbers[3 * k], points.numbers[3 * k + 1],
                                                              points.numbers[3 * k + 2], 1.0);
            positions.push_back(Vector3f((Float) q.x(), (Float) q.y(), (Float) q.z()));
        }

        /* UV primvars of this mesh: value index of every corner (file order) */
        struct Param { UVAccum *acc; uint32_t base; std::vector<uint32_t> perCorner; };
        std::vector<Param> params;
        if (wantUVs) {
            for (const Property &prop : p.properties) {
                std::string name;
                if (!uv_primvar(prop, name))
                    continue;
                const std::string interp = interpolation(prop, "constant");
                if (interp == "constant")
                    continue;
                const Value values = first_value(layer, prop);
                const size_t nValues = values.numbers.size() / 2;
                if (values.tuple != 2 || nValues == 0)
                    continue;
                std::vector<uint32_t> items;
                if (const Property *ip = p.property(prop.name + ":indices")) {
                    for (double x : first_value(layer, *ip).numbers)
                        items.push_back(x < 0 ? 0xffffffffu : (uint32_t) x);
                } else {
                    for (size_t k = 0; k < nValues; ++k)
                        items.push_back((uint32_t) k);
                }
                const size_t nItems = interp == "faceVarying" ? corners.numbers.size()
                                    : interp == "uniform" ? counts.numbers.size() : nPoints;
                bool ok = items.size() == nItems;
                for (uint32_t x : items)
                    ok = ok && x < nValues;
                if (!ok) {
                    cout << "Warning: UV set \"" << name << "\" of \"" << p.path
                         << "\" does not match its faces, ignored" << endl;
                    continue;
                }
                Param prm;
                prm.perCorner.resize(corners.numbers.size());
                size_t c = 0;
                for (size_t f = 0; f < counts.numbers.size(); ++f)
                    for (uint32_t k = 0; k < (uint32_t) counts.numbers[f]; ++k, ++c)
                        prm.perCorner[c] = interp == "faceVarying" ? items[c]
                                         : interp == "uniform" ? items[f] : items[(size_t) corners.numbers[c]];
                UVAccum *acc = nullptr;
                for (UVAccum &u : uvs)
                    if (u.name == name)
                        acc = &u;
                if (!acc) {
                    uvs.push_back(UVAccum());
                    acc = &uvs.back();
                    acc->name = name;
                }
                if (acc->meshes != meshes)
                    continue;   /* missing on an earlier mesh: dropped anyway */
                acc->meshes++;
                prm.base = (uint32_t) (acc->values.size() / 2);
                for (double x : values.numbers)
                    acc->values.push_back((float) x);
                prm.acc = acc;
                params.push_back(std::move(prm));
            }
        }

        size_t offset = 0;
        for (double cd : counts.numbers) {
            const uint32_t count = (uint32_t) cd;
            if (count < 3) {
                ++skippedFaces;
            } else {
                for (uint32_t k = 0; k < count; ++k) {
                    const size_t c = offset + (leftHanded ? count - 1 - k : k);
                    indices.push_back((uint32_t) (base + (uint64_t) corners.numbers[c]));
                    for (Param &prm : params)
                        prm.acc->corners.push_back(prm.base + prm.perCorner[c]);
                }
                sizes.push_back(count);
            }
            offset += count;
        }
        ++meshes;
    }

    std::vector<UVSet> take_uvs(const std::string &source) {
        std::vector<UVSet> result;
        for (UVAccum &a : uvs) {
            if (a.meshes != meshes) {
                cout << "Warning: UV set \"" << a.name << "\" is not on every mesh of \"" << source
                     << "\", ignored" << endl;
                continue;
            }
            UVSet set;
            set.name = a.name;
            set.values = Eigen::Map<const MatrixXf>(a.values.data(), 2, (std::ptrdiff_t) (a.values.size() / 2));
            set.corners.swap(a.corners);
            result.push_back(std::move(set));
        }
        uvs.clear();
        return result;
    }
};

} // namespace

std::vector<abc::MeshSummary> list_meshes(const Layer &layer) {
    std::vector<abc::MeshSummary> result;
    Walker w { layer, nullptr, nullptr };
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool instanced) {
        abc::MeshSummary m;
        m.path = p.path;
        m.world = world;
        m.instanced = instanced;
        const Property *pp = p.property("points"), *pc = p.property("faceVertexCounts"),
                       *pi = p.property("faceVertexIndices");
        if (pp) {
            m.vertices = layer.count(*pp);
            if (m.vertices == 0 && pp->hasTimeSamples)
                m.vertices = first_value(layer, *pp).size();
        }
        if (pc) {
            m.faces = layer.count(*pc);
            if (m.faces == 0 && pc->hasTimeSamples)
                m.faces = first_value(layer, *pc).size();
        }
        m.animated = (pp && pp->hasTimeSamples) || (pc && pc->hasTimeSamples) || (pi && pi->hasTimeSamples);
        result.push_back(m);
    };
    w.run();
    return result;
}

void load_mesh(const Layer &layer, const std::string &path, MatrixXu &F, MatrixXf &V, uint64_t *polygons,
               std::vector<UVSet> *uvs) {
    Collector c { layer };
    c.wantUVs = uvs != nullptr;
    bool found = false;
    Walker w { layer, nullptr, nullptr };
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool) {
        if (p.path == path && !found) {
            found = true;
            c.add(p, world);
        }
    };
    w.run();
    if (!found)
        fail(layer, "no polygon mesh at \"" + path + "\"");
    if (uvs)
        *uvs = c.take_uvs(layer.filename() + ":" + path);
    build_mesh(c.positions, c.sizes, c.indices, F, V, layer.filename() + ":" + path, uvs);
    if (polygons)
        *polygons = c.sizes.size();
}

void load_all(const Layer &layer, MatrixXu &F, MatrixXf &V, uint64_t *polygons, std::vector<UVSet> *uvs) {
    Collector c { layer };
    c.wantUVs = uvs != nullptr;
    size_t instanced = 0;
    Walker w { layer, nullptr, nullptr };
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool inst) {
        if (inst)
            ++instanced;
        else
            c.add(p, world);
    };
    w.run();
    if (c.meshes == 0)
        fail(layer, instanced ? "contains only instanced meshes" : "contains no polygon mesh");
    if (instanced)
        cout << "Warning: " << instanced << " instanced meshes skipped" << endl;
    if (uvs)
        *uvs = c.take_uvs(layer.filename());
    build_mesh(c.positions, c.sizes, c.indices, F, V, layer.filename(), uvs);
    if (polygons)
        *polygons = c.sizes.size();
    if (c.skippedFaces > 0)
        cout << "Warning: skipped " << c.skippedFaces << " degenerate faces with fewer than 3 vertices" << endl;
}

/* ------------------------------------------------------------------------- */
/*  Writing                                                                  */
/* ------------------------------------------------------------------------- */

namespace {

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

std::string quote(const std::string &s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\')
            out += '\\';
        if (c == '\n') {
            out += "\\n";
            continue;
        }
        out += c;
    }
    return out + "\"";
}

std::string num(double x) {
    char b[40];
    snprintf(b, sizeof b, "%.9g", x);
    return b;
}

/* The geometry of a replacement, ready to write: local points (only the
   used ones), counter-clockwise faces, indexed UV sets */
struct MeshOut {
    std::vector<float> P;
    std::vector<int32_t> counts, indices;
    struct UV { std::string name; std::vector<float> values; std::vector<uint32_t> indices; };
    std::vector<UV> uvs;
    double lo[3] = { DBL_MAX, DBL_MAX, DBL_MAX }, hi[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
};

MeshOut mesh_out(const abc::Replacement &r, const Mat4 &toLocal) {
    MatrixXu Ff;
    MatrixXf Vf;
    std::vector<CornerUVs> uvf;
    const MatrixXu *F = &r.F;
    const MatrixXf *V = &r.V;
    const std::vector<CornerUVs> *uvs = &r.uvs;
    if (r.fetch) {
        r.fetch(Ff, Vf, uvf);
        F = &Ff;
        V = &Vf;
        uvs = &uvf;
    }
    MeshOut m;
    std::vector<uint32_t> sizes, ccw, faceIds, cornerIds;
    extracted_polygons(*F, sizes, ccw, faceIds, &cornerIds);
    if (sizes.empty())
        throw std::runtime_error("USD writer: the new mesh of \"" + r.path + "\" is empty!");
    std::vector<int32_t> remap((size_t) V->cols(), -1);
    for (uint32_t i : ccw) {
        if (i >= V->cols())
            throw std::runtime_error("USD writer: vertex index out of range!");
        remap[i] = 0;
    }
    int32_t used = 0;
    for (uint32_t i = 0; i < (uint32_t) V->cols(); ++i) {
        if (remap[i] < 0)
            continue;
        remap[i] = used++;
        const Eigen::Vector4d q = toLocal * Eigen::Vector4d((*V)(0, i), (*V)(1, i), (*V)(2, i), 1.0);
        for (int k = 0; k < 3; ++k) {
            const float x = (float) q[k];
            if (!std::isfinite(x))
                throw std::runtime_error("USD writer: invalid vertex position!");
            m.P.push_back(x);
            m.lo[k] = std::min(m.lo[k], (double) x);
            m.hi[k] = std::max(m.hi[k], (double) x);
        }
    }
    for (uint32_t s : sizes)
        m.counts.push_back((int32_t) s);
    for (uint32_t i : ccw)
        m.indices.push_back(remap[i]);
    for (const CornerUVs &set : *uvs) {
        MeshOut::UV uv;
        uv.name = set.name.empty() ? "st" : set.name;
        indexed_uvs(set, cornerIds, uv.values, uv.indices);
        m.uvs.push_back(std::move(uv));
    }
    return m;
}

template <typename T, typename Fn>
void write_array(std::ostream &os, const std::vector<T> &v, size_t tuple, Fn item) {
    os << "[";
    for (size_t k = 0; k < v.size(); k += tuple) {
        if (k)
            os << ", ";
        if (k && k % (tuple * 8) == 0)
            os << "\n            ";
        if (tuple > 1)
            os << "(";
        for (size_t j = 0; j < tuple; ++j)
            os << (j ? ", " : "") << item(v[k + j]);
        if (tuple > 1)
            os << ")";
    }
    os << "]";
}

/* The new geometry of a mesh, as properties (indent: inside its prim) */
void write_geometry(std::ostream &os, const MeshOut &m, const std::string &ind,
                    const std::map<std::string, std::string> &uvTypes) {
    os << ind << "int[] faceVertexCounts = ";
    write_array(os, m.counts, 1, [](int32_t x) { return std::to_string(x); });
    os << "\n" << ind << "int[] faceVertexIndices = ";
    write_array(os, m.indices, 1, [](int32_t x) { return std::to_string(x); });
    os << "\n" << ind << "point3f[] points = ";
    write_array(os, m.P, 3, [](float x) { return num(x); });
    os << "\n" << ind << "float3[] extent = [(" << num((float) m.lo[0]) << ", " << num((float) m.lo[1]) << ", "
       << num((float) m.lo[2]) << "), (" << num((float) m.hi[0]) << ", " << num((float) m.hi[1]) << ", "
       << num((float) m.hi[2]) << ")]\n";
    os << ind << "uniform token orientation = \"rightHanded\"\n";
    for (const MeshOut::UV &uv : m.uvs) {
        auto it = uvTypes.find(uv.name);
        const std::string type = it != uvTypes.end() ? it->second : "texCoord2f[]";
        os << ind << type << " primvars:" << uv.name << " = ";
        write_array(os, uv.values, 2, [](float x) { return num(x); });
        os << " (\n" << ind << "    interpolation = \"faceVarying\"\n" << ind << ")\n";
        os << ind << "int[] primvars:" << uv.name << ":indices = ";
        write_array(os, uv.indices, 1, [](uint32_t x) { return std::to_string(x); });
        os << "\n";
    }
}

/* One prim of the override tree */
struct Node {
    std::map<std::string, Node> children;
    std::vector<std::string> order;
    const abc::Replacement *replacement = nullptr;
    Mat4 toLocal = Mat4::Identity();
};

} // namespace

void write_overlay(const Layer &layer, const std::string &output, const std::vector<abc::Replacement> &replacements) {
    Timer<> timer;
    cout << "Writing \"" << output << "\" (" << replacements.size() << " mesh"
         << (replacements.size() > 1 ? "es" : "") << " replaced, over \"" << layer.filename() << "\") .. ";
    cout.flush();

    /* The meshes, their world matrix */
    std::map<std::string, abc::MeshSummary> meshes;
    for (const abc::MeshSummary &m : list_meshes(layer))
        meshes[m.path] = m;
    Node root;
    for (const abc::Replacement &r : replacements) {
        auto it = meshes.find(r.path);
        if (it == meshes.end())
            fail(layer, "no polygon mesh at \"" + r.path + "\"");
        if (it->second.instanced)
            fail(layer, "\"" + r.path + "\" is instanced: its geometry cannot be replaced");
        if (it->second.animated)
            fail(layer, "\"" + r.path + "\" is animated: its geometry cannot be replaced");
        const double det = it->second.world.topLeftCorner<3, 3>().determinant();
        if (!std::isfinite(det) || std::abs(det) < 1e-12)
            fail(layer, "\"" + r.path + "\" has a degenerate transform (zero scale)");
        Node *n = &root;
        for (const std::string &name : str_tokenize(r.path, '/', false)) {
            if (!n->children.count(name))
                n->order.push_back(name);
            n = &n->children[name];
        }
        if (n->replacement)
            fail(layer, "\"" + r.path + "\" is replaced twice");
        n->replacement = &r;
        n->toLocal = it->second.world.inverse();
    }

    /* Sublayer path, relative when the output is next to the input */
    auto dir_of = [](const std::string &p) {
        const size_t s = p.find_last_of("/\\");
        return s == std::string::npos ? std::string() : p.substr(0, s + 1);
    };
    std::string sub = layer.filename();
    if (str_tolower(dir_of(sub)) == str_tolower(dir_of(output)))
        sub = "./" + sub.substr(dir_of(sub).size());
    for (char &c : sub)
        if (c == '\\')
            c = '/';

    const std::string temp = output + ".tmp";
    struct TempGuard {
        const std::string &path;
        bool armed = true;
        ~TempGuard() { if (armed) std::remove(path.c_str()); }
    } guard { temp };
    std::vector<std::string> notes;
    {
        std::ofstream os(temp, std::ios::binary | std::ios::trunc);
        if (!os)
            throw std::runtime_error("Unable to create \"" + temp + "\"!");
        os << "#usda 1.0\n(\n";
        os << "    doc = " << quote("Instant Meshes: remeshed meshes over " + sub) << "\n";
        for (const char *key : { "defaultPrim", "upAxis" }) {
            auto it = layer.meta.find(key);
            if (it != layer.meta.end() && it->second.kind == Value::Strings)
                os << "    " << key << " = " << quote(it->second.str()) << "\n";
        }
        for (const char *key : { "metersPerUnit", "kilogramsPerUnit", "startTimeCode", "endTimeCode",
                                 "timeCodesPerSecond", "framesPerSecond" }) {
            auto it = layer.meta.find(key);
            if (it != layer.meta.end() && it->second.kind == Value::Numbers && !it->second.numbers.empty())
                os << "    " << key << " = " << num(it->second.num()) << "\n";
        }
        os << "    subLayers = [\n        @" << sub << "@\n    ]\n)\n";

        std::function<void(const Node &, const std::string &, const std::string &)> write;
        write = [&](const Node &n, const std::string &path, const std::string &ind) {
            for (const std::string &name : n.order) {
                const Node &c = n.children.at(name);
                const std::string cpath = path + "/" + name;
                const Prim *prim = layer.prim(cpath);
                std::vector<std::string> dropped;
                std::vector<const Prim *> subsets;
                std::string binding;
                std::map<std::string, std::string> uvTypes;
                MeshOut m;
                if (c.replacement) {
                    m = mesh_out(*c.replacement, c.toLocal);
                    /* GeomSubsets: deactivated, the most used material on the mesh */
                    std::map<std::string, size_t> faces;
                    size_t inSubsets = 0;
                    for (const auto &child : prim->children) {
                        if (child->type != "GeomSubset")
                            continue;
                        subsets.push_back(child.get());
                        size_t count = 0;
                        if (const Property *ip = child->property("indices"))
                            count = layer.count(*ip);
                        inSubsets += count;
                        if (const Property *b = child->property("material:binding"))
                            if (!b->targets.empty())
                                faces[b->targets[0]] += count;
                    }
                    std::string own;
                    if (const Property *b = prim->property("material:binding"))
                        if (!b->targets.empty())
                            own = b->targets[0];
                    const Property *pc = prim->property("faceVertexCounts");
                    const size_t total = pc ? layer.count(*pc) : 0;
                    if (!own.empty() && total > inSubsets)
                        faces[own] += total - inSubsets;
                    size_t best = 0;
                    for (const auto &kv : faces)
                        if (kv.second > best) {
                            best = kv.second;
                            binding = kv.first;
                        }
                    if (binding == own)
                        binding.clear();
                    /* the UV sets keep the type of the primvar they replace */
                    for (const MeshOut::UV &uv : m.uvs)
                        if (const Property *q = prim->property("primvars:" + uv.name))
                            if (!q->relationship && !q->type.empty())
                                uvTypes[uv.name] = q->type;
                }
                os << ind << "over " << quote(name);
                if (!binding.empty())
                    os << " (\n" << ind << "    prepend apiSchemas = [\"MaterialBindingAPI\"]\n" << ind << ")";
                os << "\n" << ind << "{\n";
                if (c.replacement) {
                    const std::string in = ind + "    ";
                    write_geometry(os, m, in, uvTypes);
                    std::set<std::string> written;
                    for (const MeshOut::UV &uv : m.uvs) {
                        written.insert("primvars:" + uv.name);
                        written.insert("primvars:" + uv.name + ":indices");
                    }
                    /* opinions that no longer match the new topology */
                    static const std::set<std::string> topology { "normals", "velocities", "accelerations",
                        "holeIndices", "cornerIndices", "cornerSharpnesses", "creaseIndices", "creaseLengths",
                        "creaseSharpnesses" };
                    for (const Property &q : prim->properties) {
                        if (q.relationship || written.count(q.name) || q.type.empty())
                            continue;
                        bool block = topology.count(q.name) > 0;
                        if (!block && q.name.compare(0, 9, "primvars:") == 0) {
                            std::string base = q.name;
                            if (base.size() > 8 && base.compare(base.size() - 8, 8, ":indices") == 0)
                                base.resize(base.size() - 8);
                            const Property *bp = prim->property(base);
                            block = !(bp && interpolation(*bp, "constant") == "constant");
                        }
                        if (block) {
                            os << in << q.type << " " << q.name << " = None\n";
                            if (q.name.size() < 8 || q.name.compare(q.name.size() - 8, 8, ":indices") != 0)
                                dropped.push_back(q.name);
                        }
                    }
                    if (!binding.empty())
                        os << in << "rel material:binding = <" << binding << ">\n";
                    for (const Prim *s : subsets)
                        os << in << "over " << quote(s->name) << " (\n" << in << "    active = false\n" << in
                           << ")\n" << in << "{\n" << in << "}\n";
                    if (!dropped.empty()) {
                        std::string list;
                        for (const std::string &d : dropped)
                            list += (list.empty() ? "" : ", ") + d;
                        notes.push_back(cpath + ": blocked " + list + " (no longer matching the topology)");
                    }
                    if (!subsets.empty())
                        notes.push_back(cpath + ": " + std::to_string(subsets.size()) + " GeomSubset(s) deactivated" +
                                        (binding.empty() ? std::string() : ", bound to " + binding + " (most used)"));
                }
                write(c, cpath, ind + "    ");
                os << ind << "}\n";
            }
        };
        write(root, "", "");
        os.flush();
        if (!os)
            throw std::runtime_error("Error while writing \"" + temp + "\" (disk full?)!");
    }
    replace_file(temp, output);
    guard.armed = false;
    cout << "done. (took " << timeString(timer.value()) << ")" << endl;
    for (const std::string &n : notes)
        cout << "   " << n << endl;
}

void write_usda(const std::string &filename, const MatrixXu &F, const MatrixXf &V, const std::vector<CornerUVs> &uvs) {
    Timer<> timer;
    cout << "Writing \"" << filename << "\" (V=" << V.cols() << ", F=" << F.cols() << ") .. ";
    cout.flush();
    std::string name = filename.substr(filename.find_last_of("/\\") == std::string::npos ? 0
                                       : filename.find_last_of("/\\") + 1);
    name = name.substr(0, name.rfind('.'));
    for (char &c : name)
        if (!std::isalnum((unsigned char) c) && c != '_')
            c = '_';
    if (name.empty() || std::isdigit((unsigned char) name[0]))
        name = "Mesh_" + name;
    abc::Replacement r;
    r.path = "/" + name;
    r.F = F;
    r.V = V;
    r.uvs = uvs;
    const MeshOut m = mesh_out(r, Mat4::Identity());

    const std::string temp = filename + ".tmp";
    struct TempGuard {
        const std::string &path;
        bool armed = true;
        ~TempGuard() { if (armed) std::remove(path.c_str()); }
    } guard { temp };
    {
        std::ofstream os(temp, std::ios::binary | std::ios::trunc);
        if (!os)
            throw std::runtime_error("Unable to create \"" + temp + "\"!");
        os << "#usda 1.0\n(\n    defaultPrim = " << quote(name) << "\n    doc = \"Instant Meshes\"\n"
           << "    upAxis = \"Y\"\n)\n\n";
        os << "def Xform " << quote(name) << " (\n    kind = \"component\"\n)\n{\n";
        os << "    def Mesh " << quote(name) << "\n    {\n";
        write_geometry(os, m, "        ", std::map<std::string, std::string>());
        os << "        uniform token subdivisionScheme = \"none\"\n";
        os << "    }\n}\n";
        os.flush();
        if (!os)
            throw std::runtime_error("Error while writing \"" + temp + "\" (disk full?)!");
    }
    replace_file(temp, filename);
    guard.armed = false;
    cout << "done. (took " << timeString(timer.value()) << ")" << endl;
}

} // namespace usd
