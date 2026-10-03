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

/* The purpose authored on a prim: false when none ('value': "" for "default") */
bool authored_purpose(const Layer &layer, const Prim &p, std::string &value) {
    const Property *q = p.property("purpose");
    if (!q || q->relationship)
        return false;
    const Value v = first_value(layer, *q);
    if (v.kind != Value::Strings)
        return false;
    value = v.str() == "default" ? std::string() : v.str();
    return true;
}

/* Computed purpose (UsdGeomImageable): the prim's own when authored, else
   its parent's ("" = default) */
std::string child_purpose(const Layer &layer, const Prim &p, const std::string &parent) {
    std::string own;
    return authored_purpose(layer, p, own) ? own : parent;
}

/* Walks the defined, active prims with their world matrix */
struct Walker {
    const Layer &layer;
    std::function<void(const Prim &, const Mat4 &, bool instanced, const std::string &purpose)> onMesh;
    std::function<void(const Prim &, const Mat4 &)> onPrim;
    size_t visits = 0;

    void run() {
        for (const auto &c : layer.root.children)
            visit(*c, Mat4::Identity(), false, std::string(), 0);
    }

    void visit(const Prim &p, const Mat4 &parent, bool instanced, const std::string &parentPurpose, int depth) {
        if (p.specifier != Specifier::Def || !active(p))
            return;
        if (depth > 1000 || ++visits > 100000000)
            fail(layer, "prims nested too deeply or too many prims");
        bool reset, animated = false;
        const Mat4 local = local_transform(layer, p, reset, animated);
        const Mat4 world = reset ? local : Mat4(parent * local);
        const bool inst = instanced || instanceable(p) || p.type == "PointInstancer";
        const std::string purpose = child_purpose(layer, p, parentPurpose);
        if (onPrim)
            onPrim(p, world);
        if (p.type == "Mesh" && onMesh)
            onMesh(p, world, inst, purpose);
        for (const auto &c : p.children)
            visit(*c, world, inst, purpose, depth + 1);
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
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool instanced, const std::string &purpose) {
        abc::MeshSummary m;
        m.path = p.path;
        m.world = world;
        m.instanced = instanced;
        m.purpose = purpose;
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
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool, const std::string &) {
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
    w.onMesh = [&](const Prim &p, const Mat4 &world, bool inst, const std::string &) {
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

/* Sublayer path of the input, relative when the output is next to it */
std::string sublayer_path(const Layer &layer, const std::string &output) {
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
    return sub;
}

/* Layer header: the stage metadata of the input, the input as sublayer */
void write_header(std::ostream &os, const Layer &layer, const std::string &sub, const std::string &doc) {
    os << "#usda 1.0\n(\n";
    os << "    doc = " << quote(doc) << "\n";
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
}

/* The material bound to most faces of a mesh: its own binding and those of
   its GeomSubsets, by face count ('own': the mesh's own binding) */
std::string most_used_material(const Layer &layer, const Prim &prim, std::string &own,
                               std::vector<const Prim *> &subsets) {
    std::map<std::string, size_t> faces;
    size_t inSubsets = 0;
    for (const auto &child : prim.children) {
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
    own.clear();
    if (const Property *b = prim.property("material:binding"))
        if (!b->targets.empty())
            own = b->targets[0];
    const Property *pc = prim.property("faceVertexCounts");
    const size_t total = pc ? layer.count(*pc) : 0;
    if (!own.empty() && total > inSubsets)
        faces[own] += total - inSubsets;
    std::string best;
    size_t most = 0;
    for (const auto &kv : faces)
        if (kv.second > most) {
            most = kv.second;
            best = kv.first;
        }
    return best;
}

/* The UV sets keep the type of the primvar of the same name on 'prim' */
std::map<std::string, std::string> uv_types(const Prim &prim, const MeshOut &m) {
    std::map<std::string, std::string> types;
    for (const MeshOut::UV &uv : m.uvs)
        if (const Property *q = prim.property("primvars:" + uv.name))
            if (!q->relationship && !q->type.empty())
                types[uv.name] = q->type;
    return types;
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

    const std::string sub = sublayer_path(layer, output);
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
        write_header(os, layer, sub, "Instant Meshes: remeshed meshes over " + sub);

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
                    std::string own;
                    binding = most_used_material(layer, *prim, own, subsets);
                    if (binding == own)
                        binding.clear();
                    uvTypes = uv_types(*prim, m);
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

/* ------------------------------------------------------------------------- */
/*  Proxies                                                                  */
/* ------------------------------------------------------------------------- */

namespace {

std::string parent_of(const std::string &path) {
    const size_t s = path.rfind('/');
    return s == 0 || s == std::string::npos ? std::string() : path.substr(0, s);
}

/* Computed purpose of the prim at 'path', from what the layer authors on it
   and its ancestors ("" = default) */
std::string computed_purpose(const Layer &layer, const std::string &path) {
    std::string purpose, prefix;
    for (const std::string &name : str_tokenize(path, '/', false)) {
        prefix += "/" + name;
        const Prim *p = layer.prim(prefix);
        if (!p)
            break;
        purpose = child_purpose(layer, *p, purpose);
    }
    return purpose;
}

/* Where the proxy of a mesh goes */
struct Placement {
    std::string proxy;        ///< path of the proxy mesh
    std::string renderScope;  ///< geo/render convention: the "render" prim; empty: next to the mesh
    std::string proxyScope;   ///< geo/render convention: the "proxy" prim mirroring it

    /* The prim of the input a prim of the proxy path mirrors (its
       transform is copied); "" for the prims above the mirror */
    std::string source(const std::string &mesh, const std::string &path) const {
        if (path == proxy)
            return mesh;
        if (!proxyScope.empty() && (path == proxyScope || path.compare(0, proxyScope.size() + 1, proxyScope + "/") == 0))
            return renderScope + path.substr(proxyScope.size());
        return std::string();
    }
};

Placement place_proxy(const std::string &mesh) {
    const std::vector<std::string> segs = str_tokenize(mesh, '/', false);
    Placement pl;
    /* .../geo/render/<path>  ->  .../geo/proxy/<path>: never below the
       render scope, that some importers skip whole (Blender) */
    for (size_t k = segs.size() - 1; k-- > 1;) {
        if (segs[k] != "render")
            continue;
        std::string geo;
        for (size_t j = 0; j < k; ++j)
            geo += "/" + segs[j];
        pl.renderScope = geo + "/render";
        pl.proxyScope = geo + "/proxy";
        pl.proxy = pl.proxyScope;
        for (size_t j = k + 1; j < segs.size(); ++j)
            pl.proxy += "/" + segs[j];
        return pl;
    }
    /* <name>_proxy, a sibling: same parent transforms */
    pl.proxy = parent_of(mesh) + "/" + segs.back() + "_proxy";
    return pl;
}

std::string num17(double x) {
    char b[40];
    snprintf(b, sizeof b, "%.17g", x);
    return b;
}

/* A transform op value as text (scalars, vectors, quaternions, matrices,
   token arrays) */
std::string value_text(const Value &v, const std::string &type) {
    if (v.kind == Value::Blocked)
        return "None";
    const bool array = type.size() > 2 && type.compare(type.size() - 2, 2, "[]") == 0;
    if (v.kind == Value::Strings) {
        if (!array)
            return quote(v.str());
        std::string s = "[";
        for (size_t i = 0; i < v.strings.size(); ++i)
            s += (i ? ", " : "") + quote(v.strings[i]);
        return s + "]";
    }
    auto tuple = [&](size_t at, size_t n) {
        if (n == 1)
            return num17(at < v.numbers.size() ? v.numbers[at] : 0);
        std::string s = "(";
        for (size_t k = 0; k < n; ++k)
            s += (k ? ", " : "") + num17(at + k < v.numbers.size() ? v.numbers[at + k] : 0);
        return s + ")";
    };
    auto element = [&](size_t at) {
        if (v.tuple == 16 || v.tuple == 9 || v.tuple == 4 && type.compare(0, 8, "matrix2d") == 0) {
            const size_t n = v.tuple == 16 ? 4 : v.tuple == 9 ? 3 : 2;
            std::string s = "(";
            for (size_t r = 0; r < n; ++r)
                s += (r ? ", " : "") + tuple(at + r * n, n);
            return s + ")";
        }
        return tuple(at, (size_t) v.tuple);
    };
    if (!array)
        return element(0);
    std::string s = "[";
    for (size_t at = 0; at < v.numbers.size(); at += (size_t) v.tuple)
        s += (at ? ", " : "") + element(at);
    return s + "]";
}

/* The transform of a prim, copied as properties: xformOpOrder and its ops,
   default values and time samples */
void copy_transform(const Layer &layer, const Prim &prim, std::vector<std::string> &lines) {
    const Property *order = prim.property("xformOpOrder");
    if (!order)
        return;
    const Value ops = first_value(layer, *order);
    lines.push_back("uniform token[] xformOpOrder = " + value_text(ops, "token[]"));
    std::set<std::string> done;
    for (const std::string &entry : ops.strings) {
        const std::string name = entry.compare(0, 8, "!invert!") == 0 ? entry.substr(8) : entry;
        const Property *op = prim.property(name);
        if (!op || op->relationship || !done.insert(name).second)
            continue;
        const std::string decl = std::string(op->uniform ? "uniform " : "") + op->type + " " + name;
        const Value d = layer.value(*op);
        if (d.kind == Value::Numbers || d.kind == Value::Blocked)
            lines.push_back(decl + " = " + value_text(d, op->type));
        if (op->hasTimeSamples) {
            std::string t = decl + ".timeSamples = { ";
            for (const auto &s : layer.samples(*op).samples)
                t += num17(s.first) + ": " + value_text(s.second, op->type) + ", ";
            lines.push_back(t + "}");
        }
    }
}

/* World matrix of a prim of the proxy path: the input's prims where they
   exist, else the prims they mirror (first frame) */
Mat4 proxy_world(const Layer &layer, const Placement &pl, const std::string &mesh, const std::string &path) {
    Mat4 M = Mat4::Identity();
    std::string prefix;
    for (const std::string &name : str_tokenize(path, '/', false)) {
        prefix += "/" + name;
        const Prim *p = layer.prim(prefix);
        if (!p) {
            const std::string src = pl.source(mesh, prefix);
            p = src.empty() ? nullptr : layer.prim(src);
        }
        if (!p)
            continue;
        bool reset, animated = false;
        const Mat4 L = local_transform(layer, *p, reset, animated);
        M = reset ? L : Mat4(M * L);
    }
    return M;
}

std::vector<Placement> place_proxies(const Layer &layer, const std::vector<std::string> &meshes) {
    std::vector<Placement> result;
    std::set<std::string> taken;
    for (const std::string &m : meshes) {
        const std::string purpose = computed_purpose(layer, m);
        if (purpose == "proxy" || purpose == "guide")
            fail(layer, "\"" + m + "\" has the purpose \"" + purpose + "\": it cannot get a proxy");
        Placement pl = place_proxy(m);
        if (layer.prim(pl.proxy))
            fail(layer, "\"" + pl.proxy + "\", the proxy of \"" + m + "\", already exists (a proxy made earlier?)");
        if (!taken.insert(pl.proxy).second)
            fail(layer, "two meshes would get the same proxy \"" + pl.proxy + "\"");
        result.push_back(pl);
    }
    return result;
}

/* The material of a mesh: the most used one of the mesh and its subsets,
   else the nearest binding of an ancestor */
std::string bound_material(const Layer &layer, const Prim &mesh) {
    std::string own;
    std::vector<const Prim *> subsets;
    std::string best = most_used_material(layer, mesh, own, subsets);
    for (std::string p = parent_of(mesh.path); best.empty() && !p.empty(); p = parent_of(p))
        if (const Prim *a = layer.prim(p))
            if (const Property *b = a->property("material:binding"))
                if (!b->targets.empty())
                    best = b->targets[0];
    return best;
}

/* One prim of the proxy layer: an over, or a prim to define */
struct ProxyNode {
    std::map<std::string, ProxyNode> children;
    std::vector<std::string> order;
    std::string type;                     ///< prim to define ("Scope", "Xform", "Mesh"); empty: over
    std::vector<std::string> lines;       ///< properties
    std::string binding;
    const abc::Replacement *mesh = nullptr;
    const Prim *source = nullptr;
    Mat4 toLocal = Mat4::Identity();
};

} // namespace

std::vector<std::string> proxy_paths(const Layer &layer, const std::vector<std::string> &meshes) {
    std::vector<std::string> result;
    for (const Placement &pl : place_proxies(layer, meshes))
        result.push_back(pl.proxy);
    return result;
}

void write_proxies(const Layer &layer, const std::string &output, const std::vector<abc::Replacement> &proxies) {
    Timer<> timer;
    cout << "Writing \"" << output << "\" (" << proxies.size() << " prox" << (proxies.size() > 1 ? "ies" : "y")
         << ", over \"" << layer.filename() << "\") .. ";
    cout.flush();

    std::map<std::string, abc::MeshSummary> meshes;
    for (const abc::MeshSummary &m : list_meshes(layer))
        meshes[m.path] = m;
    std::vector<std::string> paths;
    for (const abc::Replacement &r : proxies) {
        auto it = meshes.find(r.path);
        if (it == meshes.end())
            fail(layer, "no polygon mesh at \"" + r.path + "\"");
        if (it->second.instanced)
            fail(layer, "\"" + r.path + "\" is instanced: no proxy is made for it");
        if (it->second.animated)
            fail(layer, "\"" + r.path + "\" is animated: a proxy could not follow it");
        paths.push_back(r.path);
    }
    const std::vector<Placement> places = place_proxies(layer, paths);

    ProxyNode root;
    auto node = [&](const std::string &path) -> ProxyNode & {
        ProxyNode *n = &root;
        for (const std::string &name : str_tokenize(path, '/', false)) {
            if (!n->children.count(name))
                n->order.push_back(name);
            n = &n->children[name];
        }
        return *n;
    };
    std::set<std::string> renderSet;
    std::vector<std::string> notes;
    for (size_t i = 0; i < proxies.size(); ++i) {
        const abc::Replacement &r = proxies[i];
        const Placement &pl = places[i];
        const Prim *src = layer.prim(r.path);

        /* the render side: purpose "render" on the render scope (on the mesh
           too when a purpose authored below the scope would hide it), the
           proxyPrim relationship */
        std::vector<std::string> renderAt { pl.renderScope.empty() ? r.path : pl.renderScope };
        if (!pl.renderScope.empty()) {
            std::string own;
            for (std::string p = r.path; p.size() > pl.renderScope.size(); p = parent_of(p))
                if (const Prim *q = layer.prim(p))
                    if (authored_purpose(layer, *q, own)) {
                        renderAt.push_back(r.path);
                        break;
                    }
        }
        for (const std::string &at : renderAt)
            if (computed_purpose(layer, at) != "render" && renderSet.insert(at).second)
                node(at).lines.push_back("uniform token purpose = \"render\"");
        node(r.path).lines.push_back("rel proxyPrim = <" + pl.proxy + ">");

        /* the proxy: the prims missing on its path are defined, with the
           transforms of the prims they mirror (animation included) */
        std::string prefix;
        for (const std::string &name : str_tokenize(pl.proxy, '/', false)) {
            prefix += "/" + name;
            ProxyNode &n = node(prefix);
            if (layer.prim(prefix) || !n.type.empty())
                continue;
            const Prim *mirror = layer.prim(pl.source(r.path, prefix));
            if (prefix == pl.proxy)
                n.type = "Mesh";
            else
                n.type = mirror && mirror->type == "Scope" ? "Scope" : "Xform";
            if (prefix == pl.proxyScope)
                n.lines.push_back("uniform token purpose = \"proxy\"");
            if (mirror && n.type != "Scope")
                copy_transform(layer, *mirror, n.lines);
        }
        const Mat4 world = proxy_world(layer, pl, r.path, pl.proxy);
        const double det = world.topLeftCorner<3, 3>().determinant();
        if (!std::isfinite(det) || std::abs(det) < 1e-12)
            fail(layer, "\"" + pl.proxy + "\" would have a degenerate transform (zero scale)");
        ProxyNode &n = node(pl.proxy);
        n.mesh = &r;
        n.source = src;
        n.toLocal = world.inverse();
        n.binding = bound_material(layer, *src);
        n.lines.push_back("uniform token purpose = \"proxy\"");
        n.lines.push_back("uniform token subdivisionScheme = \"none\"");
        if (const Property *d = src->property("doubleSided"))
            if (!d->relationship && layer.value(*d).kind == Value::Numbers)
                n.lines.push_back(std::string("uniform bool doubleSided = ") +
                                  (layer.value(*d).num() != 0 ? "true" : "false"));
        notes.push_back(r.path + " -> " + pl.proxy + (n.binding.empty() ? std::string() : " (" + n.binding + ")"));
    }

    const std::string sub = sublayer_path(layer, output);
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
        write_header(os, layer, sub, "Instant Meshes: proxies over " + sub);
        std::function<void(const ProxyNode &, const std::string &)> write;
        write = [&](const ProxyNode &n, const std::string &ind) {
            for (const std::string &name : n.order) {
                const ProxyNode &c = n.children.at(name);
                os << ind << (c.type.empty() ? "over" : "def " + c.type) << " " << quote(name);
                if (!c.binding.empty())
                    os << " (\n" << ind << "    prepend apiSchemas = [\"MaterialBindingAPI\"]\n" << ind << ")";
                os << "\n" << ind << "{\n";
                const std::string in = ind + "    ";
                if (c.mesh) {
                    const MeshOut m = mesh_out(*c.mesh, c.toLocal);
                    write_geometry(os, m, in, uv_types(*c.source, m));
                }
                for (const std::string &line : c.lines)
                    os << in << line << "\n";
                if (!c.binding.empty())
                    os << in << "rel material:binding = <" << c.binding << ">\n";
                write(c, in);
                os << ind << "}\n";
            }
        };
        write(root, "");
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

SceneUnits stage_units(const Layer &layer) {
    SceneUnits units;
    auto mpu = layer.meta.find("metersPerUnit");
    units.metersPerUnit = mpu != layer.meta.end() && mpu->second.kind == Value::Numbers && !mpu->second.numbers.empty()
                        ? mpu->second.num() : 0.01;
    auto up = layer.meta.find("upAxis");
    units.upAxis = up != layer.meta.end() && up->second.kind == Value::Strings && up->second.str() == "Z" ? "Z" : "Y";
    return units;
}

void write_usda(const std::string &filename, const MatrixXu &F, const MatrixXf &V, const std::vector<CornerUVs> &uvs,
                const SceneUnits &units) {
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
        /* without metersPerUnit, USD readers take centimeters */
        os << "#usda 1.0\n(\n    defaultPrim = " << quote(name) << "\n    doc = \"Instant Meshes\"\n"
           << "    metersPerUnit = " << num17(units.metersPerUnit) << "\n"
           << "    upAxis = " << quote(units.upAxis) << "\n)\n\n";
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
