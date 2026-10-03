/*
    abc.cpp: Alembic (.abc) polygon mesh support (see abc.h)
*/

#include "abc.h"
#include <deque>
#include "meshio.h"
#include <Eigen/Geometry>

namespace abc {

using ogawa::is_data;

/// Maximum object nesting (instances included) accepted during a traversal
static const uint32_t MAX_OBJECT_DEPTH = 1024;
/// Maximum number of meshes / objects expanded through instancing
static const uint64_t MAX_MESHES = 1000000;
static const uint64_t MAX_VISITS = 10000000;

uint32_t pod_size(Pod pod) {
    static const uint32_t sizes[PodCount] = { 1, 1, 1, 2, 2, 4, 4, 8, 8, 2, 4, 8, 0, 0 };
    return pod < PodCount ? sizes[pod] : 0;
}

std::string Object::schema() const {
    auto it = meta.find("schema");
    return it == meta.end() ? std::string() : it->second;
}

/* ------------------------------------------------------------------------- */
/*  Bounds-checked parsing of header buffers                                 */
/* ------------------------------------------------------------------------- */

namespace {

class Cursor {
public:
    Cursor(const Archive &archive, const std::vector<uint8_t> &buf, size_t end,
           const char *what)
        : mArchive(archive), mBuf(buf), mEnd(std::min(end, buf.size())), mWhat(what) { }

    bool done() const { return mPos >= mEnd; }
    size_t pos() const { return mPos; }

    template <typename T> T get() {
        need(sizeof(T));
        T value;
        memcpy(&value, mBuf.data() + mPos, sizeof(T));
        mPos += sizeof(T);
        return value;
    }

    std::string str(size_t size) {
        need(size);
        std::string s((const char *) mBuf.data() + mPos, size);
        mPos += size;
        return s;
    }

    /* Integer whose width is given by a 2-bit hint (1, 2 or 4 bytes) */
    uint32_t hinted(uint32_t hint) {
        switch (hint) {
            case 0: return get<uint8_t>();
            case 1: return get<uint16_t>();
            case 2: return get<uint32_t>();
            default: mArchive.fail(std::string("invalid integer width in ") + mWhat);
        }
    }

private:
    void need(size_t size) {
        if (size > mEnd - mPos)
            mArchive.fail(std::string("truncated ") + mWhat);
    }

    const Archive &mArchive;
    const std::vector<uint8_t> &mBuf;
    size_t mPos = 0, mEnd;
    const char *mWhat;
};

} // namespace

/* ------------------------------------------------------------------------- */
/*  Archive                                                                  */
/* ------------------------------------------------------------------------- */

Archive::Archive(const std::string &filename) : mIn(filename) {
    std::vector<uint64_t> root = mIn.group(mIn.root());
    if (root.size() < 6 || !is_data(root[0]) || !is_data(root[1]) || is_data(root[2]) ||
        !is_data(root[3]) || !is_data(root[4]) || !is_data(root[5]))
        fail("not an Alembic archive (unexpected root layout)");

    std::vector<uint8_t> version = mIn.data(root[0]);
    int32_t formatVersion = -1;
    if (version.size() == 4)
        memcpy(&formatVersion, version.data(), 4);
    if (formatVersion != 0)
        fail("unsupported Alembic format version " + std::to_string(formatVersion));

    /* Indexed metadata table; index 0 is the empty metadata */
    mIndexedMeta.push_back(MetaData());
    if (mIn.data_size(root[5]) > 65536)
        fail("indexed metadata table unexpectedly large");
    std::vector<uint8_t> table = mIn.data(root[5]);
    Cursor c(*this, table, table.size(), "indexed metadata");
    while (!c.done()) {
        const uint8_t size = c.get<uint8_t>();
        mIndexedRaw.push_back(c.str(size));
        mIndexedMeta.push_back(parse_metadata(mIndexedRaw.back()));
    }

    /* Time samplings: max sample (uint32), time per cycle (double), number
       of stored times (uint32), stored times (doubles) */
    std::vector<uint8_t> sampling = mIn.data(root[4]);
    Cursor ts(*this, sampling, sampling.size(), "time samplings");
    while (!ts.done()) {
        TimeSampling t;
        ts.get<uint32_t>();
        t.timePerCycle = ts.get<double>();
        const uint32_t count = ts.get<uint32_t>();
        if (count == 0 || count > sampling.size() / 8)
            fail("invalid time sampling");
        for (uint32_t i = 0; i < count; ++i)
            t.times.push_back(ts.get<double>());
        mTimeSamplings.push_back(t);
    }
    if (mTimeSamplings.empty())
        mTimeSamplings.push_back(TimeSampling { 1.0, { 0.0 } });

    mTop.name = "ABC";
    mTop.path = "/";
    mTop.group = root[2];
}

uint32_t Archive::stored_index(const Property &p, uint32_t index) const {
    /* Same mapping as the reference implementation (verifyIndex) */
    if (index < p.firstChanged || (p.firstChanged == 0 && p.lastChanged == 0))
        return 0;
    if (index >= p.lastChanged)
        return p.lastChanged - p.firstChanged + 1;
    return index - p.firstChanged + 1;
}

std::string serialize(const MetaData &meta) {
    std::string s;
    for (const auto &kv : meta)
        s += (s.empty() ? "" : ";") + kv.first + "=" + kv.second;
    return s;
}

MetaData Archive::parse_metadata(const std::string &text) const {
    /* "key=value;key=value", first occurrence of a key wins, parsing stops
       at a malformed pair (same tolerance as the reference implementation) */
    MetaData meta;
    size_t pos = 0;
    while (pos < text.size()) {
        const size_t pair = text.find(';', pos), assign = text.find('=', pos);
        if (assign == std::string::npos || assign > pair)
            break;
        const std::string key = text.substr(pos, assign - pos);
        const size_t end = pair == std::string::npos ? text.size() : pair;
        meta.insert(std::make_pair(key, text.substr(assign + 1, end - assign - 1)));
        if (pair == std::string::npos)
            break;
        pos = pair + 1;
    }
    return meta;
}

std::vector<Object> Archive::children(const Object &object) {
    std::vector<Object> result;
    std::vector<uint64_t> g = mIn.group(object.group);
    if (g.empty() || !is_data(g.back()))
        return result;
    std::vector<uint8_t> buf = mIn.data(g.back());
    if (buf.size() <= 32)   /* the last 32 bytes are hashes */
        return result;

    Cursor c(*this, buf, buf.size() - 32, "object headers");
    while (!c.done()) {
        const size_t start = c.pos();
        Object child;
        const uint32_t nameSize = c.get<uint32_t>();
        if (nameSize == 0)
            fail("object with an empty name");
        child.name = c.str(nameSize);
        const uint8_t metaIndex = c.get<uint8_t>();
        if (metaIndex == 0xff)
            child.meta = parse_metadata(c.str(c.get<uint32_t>()));
        else if (metaIndex < mIndexedMeta.size())
            child.meta = mIndexedMeta[metaIndex];
        else
            fail("invalid metadata index in the headers of \"" + object.path + "\"");

        /* Child i lives at index i + 1, the headers are the last entry */
        const size_t index = result.size() + 1;
        if (index + 1 >= g.size() || is_data(g[index]))
            fail("object \"" + child.name + "\" has no data");
        child.group = g[index];
        child.path = (object.path == "/" ? "" : object.path) + "/" + child.name;
        child.rawHeader.assign(buf.begin() + start, buf.begin() + c.pos());
        result.push_back(child);
    }
    return result;
}

Property Archive::properties(const Object &object) {
    Property top;
    top.name = "";
    top.type = Property::Compound;
    std::vector<uint64_t> g = mIn.group(object.group);
    if (!g.empty() && !is_data(g[0]))
        top.group = g[0];
    return top;
}

std::vector<Property> Archive::properties(const Property &compound) {
    std::vector<Property> result;
    if (compound.type != Property::Compound)
        fail("property \"" + compound.name + "\" is not a compound");
    std::vector<uint64_t> g = mIn.group(compound.group);
    if (g.empty() || !is_data(g.back()))
        return result;
    std::vector<uint8_t> buf = mIn.data(g.back());

    Cursor c(*this, buf, buf.size(), "property headers");
    while (!c.done()) {
        const size_t start = c.pos();
        Property p;
        const uint32_t info = c.get<uint32_t>();
        const uint32_t kind = info & 0x3, hint = (info & 0xc) >> 2;
        p.type = kind == 0 ? Property::Compound : (kind == 1 ? Property::Scalar : Property::Array);

        if (p.type != Property::Compound) {
            const uint32_t pod = (info & 0xf0) >> 4;
            if (pod >= PodCount)
                fail("invalid property value type " + std::to_string(pod));
            p.pod = (Pod) pod;
            p.extent = (info & 0xff000) >> 12;
            p.samples = c.hinted(hint);
            if (info & 0x200) {          /* explicit stored sample range */
                p.firstChanged = c.hinted(hint);
                p.lastChanged = c.hinted(hint);
            } else if (info & 0x800) {   /* constant: one stored sample */
                p.firstChanged = p.lastChanged = 0;
            } else {
                p.firstChanged = 1;
                p.lastChanged = p.samples > 0 ? p.samples - 1 : 0;
            }
            if (info & 0x100)
                p.timeSampling = c.hinted(hint);
        }

        const uint32_t nameSize = c.hinted(hint);
        if (nameSize == 0)
            fail("property with an empty name");
        p.name = c.str(nameSize);
        if (p.timeSampling >= mTimeSamplings.size())
            fail("property \"" + p.name + "\" uses an unknown time sampling");

        const uint32_t metaIndex = (info & 0xff00000) >> 20;
        if (metaIndex == 0xff)
            p.meta = parse_metadata(c.str(c.hinted(hint)));
        else if (metaIndex < mIndexedMeta.size())
            p.meta = mIndexedMeta[metaIndex];
        else
            fail("invalid metadata index in property \"" + p.name + "\"");

        /* Sub-property i lives at index i, the headers are the last entry */
        const size_t index = result.size();
        if (index + 1 >= g.size() || is_data(g[index]))
            fail("property \"" + p.name + "\" has no data");
        p.group = g[index];
        p.rawHeader.assign(buf.begin() + start, buf.begin() + c.pos());
        result.push_back(p);
    }
    return result;
}

bool Archive::find(const Property &compound, const std::string &name, Property &out) {
    for (const Property &p : properties(compound)) {
        if (p.name == name) {
            out = p;
            return true;
        }
    }
    return false;
}

std::vector<uint8_t> Archive::sample(const Property &property) {
    std::vector<uint8_t> result;
    if (property.type == Property::Compound)
        fail("property \"" + property.name + "\" has no values");
    if (property.samples == 0)
        return result;

    /* First sample: scalar data at [0], array data at [0] (dimensions at [1]) */
    std::vector<uint64_t> g = mIn.group(property.group);
    if (g.empty() || !is_data(g[0]))
        fail("property \"" + property.name + "\" has no sample data");
    const uint64_t size = mIn.data_size(g[0]);
    if (size == 0)
        return result;
    if (size < 16)
        fail("truncated sample in property \"" + property.name + "\"");

    const uint64_t payload = size - 16;   /* skip the 16-byte content hash */
    const uint32_t value = pod_size(property.pod);
    if (value > 0) {
        const uint64_t element = (uint64_t) value * property.extent;
        const bool ok = property.type == Property::Scalar
            ? payload == element
            : (element > 0 ? payload % element == 0 : payload == 0);
        if (!ok)
            fail("sample size mismatch in property \"" + property.name + "\"");
    }
    result.resize((size_t) payload);
    if (payload > 0)
        mIn.read_data(g[0], 16, payload, result.data());
    return result;
}

uint64_t Archive::sample_count(const Property &property) {
    if (property.type == Property::Compound)
        fail("property \"" + property.name + "\" has no values");
    if (property.samples == 0)
        return 0;
    std::vector<uint64_t> g = mIn.group(property.group);
    if (g.empty() || !is_data(g[0]))
        fail("property \"" + property.name + "\" has no sample data");
    const uint64_t size = mIn.data_size(g[0]);
    if (size == 0)
        return 0;
    if (size < 16)
        fail("truncated sample in property \"" + property.name + "\"");
    const uint64_t element = (uint64_t) pod_size(property.pod) * property.extent;
    if (element == 0 || (size - 16) % element != 0)
        fail("sample size mismatch in property \"" + property.name + "\"");
    return (size - 16) / element;
}

std::vector<double> Archive::sample_doubles(const Property &property) {
    std::vector<uint8_t> raw = sample(property);
    const uint32_t size = pod_size(property.pod);
    if (size == 0 || property.pod == PodFloat16 || property.pod == PodBool)
        fail("property \"" + property.name + "\" is not numeric");
    const size_t count = raw.size() / size;
    std::vector<double> result(count);
    const uint8_t *p = raw.data();
    for (size_t i = 0; i < count; ++i, p += size) {
        switch (property.pod) {
            case PodFloat32: { float v; memcpy(&v, p, 4); result[i] = v; } break;
            case PodFloat64: { double v; memcpy(&v, p, 8); result[i] = v; } break;
            case PodUint8:  result[i] = *p; break;
            case PodInt8:   result[i] = (int8_t) *p; break;
            case PodUint16: { uint16_t v; memcpy(&v, p, 2); result[i] = v; } break;
            case PodInt16:  { int16_t v;  memcpy(&v, p, 2); result[i] = v; } break;
            case PodUint32: { uint32_t v; memcpy(&v, p, 4); result[i] = v; } break;
            case PodInt32:  { int32_t v;  memcpy(&v, p, 4); result[i] = v; } break;
            case PodUint64: { uint64_t v; memcpy(&v, p, 8); result[i] = (double) v; } break;
            case PodInt64:  { int64_t v;  memcpy(&v, p, 8); result[i] = (double) v; } break;
            default: fail("property \"" + property.name + "\" is not numeric");
        }
    }
    return result;
}

std::vector<uint32_t> Archive::sample_indices(const Property &property) {
    std::vector<uint8_t> raw = sample(property);
    const uint32_t size = pod_size(property.pod);
    std::vector<uint32_t> result;
    if (property.pod == PodInt32 || property.pod == PodUint32) {
        result.resize(raw.size() / 4);
        if (!raw.empty())
            memcpy(result.data(), raw.data(), raw.size());
        if (property.pod == PodInt32)
            for (uint32_t v : result)
                if ((int32_t) v < 0)
                    fail("negative index in property \"" + property.name + "\"");
        return result;
    }
    if (size == 0 || property.pod == PodFloat16 || property.pod == PodFloat32 ||
        property.pod == PodFloat64 || property.pod == PodBool)
        fail("property \"" + property.name + "\" does not hold integers");
    std::vector<double> values = sample_doubles(property);
    result.resize(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
        if (values[i] < 0 || values[i] > 4294967295.0)
            fail("index out of range in property \"" + property.name + "\"");
        result[i] = (uint32_t) values[i];
    }
    return result;
}

std::string Archive::sample_string(const Property &property) {
    if (property.pod != PodString)
        fail("property \"" + property.name + "\" is not a string");
    std::vector<uint8_t> raw = sample(property);
    size_t end = 0;
    while (end < raw.size() && raw[end] != 0)
        ++end;
    return std::string((const char *) raw.data(), end);
}

bool Archive::resolve(const std::string &path, Object &out) {
    Object current = mTop;
    for (const std::string &name : str_tokenize(path, '/', false)) {
        bool found = false;
        for (const Object &child : children(current)) {
            if (child.name == name) {
                current = child;
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    out = current;
    return true;
}

/* ------------------------------------------------------------------------- */
/*  Polygon meshes                                                           */
/* ------------------------------------------------------------------------- */

namespace {

const char *SCHEMA_XFORM    = "AbcGeom_Xform_v3";
const char *SCHEMA_POLYMESH = "AbcGeom_PolyMesh_v1";
const char *SCHEMA_SUBD     = "AbcGeom_SubD_v1";

/* Local matrix of an Xform object (column vectors: p' = M p) and whether it
   inherits its parent's transform. Alembic lists the operations outermost
   first, so M = op[0] * op[1] * ... * op[n-1]. */
Eigen::Matrix4d local_transform(Archive &ar, const Object &object, bool &inherits) {
    Eigen::Matrix4d M = Eigen::Matrix4d::Identity();
    inherits = true;

    Property xform, ops, vals, inh;
    if (!ar.find(ar.properties(object), ".xform", xform))
        return M;
    if (ar.find(xform, ".inherits", inh) && inh.type == Property::Scalar) {
        std::vector<uint8_t> b = ar.sample(inh);
        if (!b.empty())
            inherits = b[0] != 0;
    }
    if (!ar.find(xform, ".ops", ops) || ops.type != Property::Scalar || ops.samples == 0)
        return M;
    if (ops.pod != PodUint8)
        ar.fail("transform operations of \"" + object.path + "\" have an invalid type");
    std::vector<uint8_t> codes = ar.sample(ops);

    std::vector<double> values;
    if (ar.find(xform, ".vals", vals))
        values = ar.sample_doubles(vals);

    static const uint32_t channels[] = { 3, 3, 4, 16, 1, 1, 1 };
    size_t needed = 0;
    for (uint8_t code : codes) {
        if ((code >> 4) > 6)
            ar.fail("unknown transform operation in \"" + object.path + "\"");
        needed += channels[code >> 4];
    }
    if (values.size() != needed)
        ar.fail("transform of \"" + object.path + "\" has " + std::to_string(values.size()) +
                " values, expected " + std::to_string(needed));
    for (double v : values)
        if (!std::isfinite(v))
            ar.fail("transform of \"" + object.path + "\" contains invalid values");

    const double deg = M_PI / 180.0;
    const double *v = values.data();
    for (uint8_t code : codes) {
        Eigen::Matrix4d C = Eigen::Matrix4d::Identity();
        switch (code >> 4) {
            case 0: /* scale */
                C(0, 0) = v[0]; C(1, 1) = v[1]; C(2, 2) = v[2];
                break;
            case 1: /* translate */
                C(0, 3) = v[0]; C(1, 3) = v[1]; C(2, 3) = v[2];
                break;
            case 2: { /* rotate around an axis, degrees */
                Eigen::Vector3d axis(v[0], v[1], v[2]);
                if (axis.norm() > 0)
                    C.topLeftCorner<3, 3>() =
                        Eigen::AngleAxisd(v[3] * deg, axis.normalized()).toRotationMatrix();
                break;
            }
            case 3: /* matrix, stored row-vector style (Imath): transpose */
                for (int j = 0; j < 4; ++j)
                    for (int k = 0; k < 4; ++k)
                        C(k, j) = v[4 * j + k];
                break;
            default: { /* rotate X / Y / Z, degrees */
                Eigen::Vector3d axis = Eigen::Vector3d::Zero();
                axis[(code >> 4) - 4] = 1;
                C.topLeftCorner<3, 3>() = Eigen::AngleAxisd(v[0] * deg, axis).toRotationMatrix();
                break;
            }
        }
        M = M * C;
        v += channels[code >> 4];
    }
    return M;
}

class MeshCollector {
public:
    /* 'filter' selects the meshes at or below a path ("" = all), or exactly
       at that path when 'exact' is set */
    MeshCollector(Archive &ar, const std::string &filter, bool geometry, bool exact = false,
                  bool uvs = false)
        : mAr(ar), mFilter(filter), mGeometry(geometry), mExact(exact), mWantUVs(uvs) { }

    void run() {
        visit(mAr.top(), "/", Eigen::Matrix4d::Identity(), 0, false);
        /* A mesh object reached several times is shared by instances */
        for (size_t i = 0; i < meshes.size(); ++i)
            if (mGroupUses[mGroups[i]] > 1)
                meshes[i].instanced = true;
    }

    std::vector<Vector3f> positions;
    std::vector<uint32_t> sizes, indices;
    std::vector<MeshSummary> meshes;
    uint64_t skippedSubD = 0, skippedFaces = 0;

    /* The UV sets found on every loaded mesh, per polygon corner (aligned
       with 'indices'); the others are dropped with a warning */
    std::vector<UVSet> take_uvs(const std::string &source) {
        std::vector<UVSet> result;
        for (UVAccum &a : mUVs) {
            if (a.meshes != meshes.size()) {
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
        mUVs.clear();
        return result;
    }

private:
    struct UVAccum {
        std::string key, name;     /* key: "" for the primary set (.geom/uv) */
        std::vector<float> values;
        std::vector<uint32_t> corners;
        size_t meshes = 0;
    };

    /* A UV parameter of one mesh: its values and, for every corner (file
       order), the index of its value */
    struct UVParam {
        std::string key, name;
        std::vector<double> values;
        std::vector<uint32_t> perCorner;
    };

    static std::string meta_of(const Property &a, const Property &b, const char *key) {
        auto it = a.meta.find(key);
        if (it != a.meta.end())
            return it->second;
        it = b.meta.find(key);
        return it != b.meta.end() ? it->second : std::string();
    }

    /* Reads a 2D GeomParam (indexed compound or plain array); false, with
       a warning, if it does not fit the mesh */
    bool read_uv_param(const Property &p, const std::string &path, const std::vector<uint32_t> &counts,
                       const std::vector<uint32_t> &corners, size_t nVertices, UVParam &out) {
        Property vals = p, indexProp;
        bool indexed = false;
        if (p.type == Property::Compound) {
            if (!mAr.find(p, ".vals", vals))
                return false;
            indexed = mAr.find(p, ".indices", indexProp);
        }
        if (vals.type != Property::Array || vals.extent != 2 ||
            (vals.pod != PodFloat32 && vals.pod != PodFloat64) || vals.samples == 0)
            return false;
        const std::string scope = meta_of(p, vals, "geoScope");

        out.values = mAr.sample_doubles(vals);
        const size_t nValues = out.values.size() / 2;
        std::vector<uint32_t> items;          /* value of each scope item */
        size_t nItems;
        if (scope == "fvr" || scope.empty())
            nItems = corners.size();
        else if (scope == "vtx" || scope == "var")
            nItems = nVertices;
        else if (scope == "uni")
            nItems = counts.size();
        else
            return false;                     /* constant: not a texture coordinate */
        if (indexed) {
            items = mAr.sample_indices(indexProp);
        } else {
            items.resize(nValues);
            for (size_t i = 0; i < nValues; ++i)
                items[i] = (uint32_t) i;
        }
        bool ok = items.size() == nItems;
        for (uint32_t i : items)
            ok = ok && i < nValues;
        if (!ok) {
            cout << "Warning: UV set \"" << out.name << "\" of \"" << path
                 << "\" does not match its faces, ignored" << endl;
            return false;
        }

        out.perCorner.resize(corners.size());
        size_t c = 0;
        for (size_t f = 0; f < counts.size(); ++f)
            for (uint32_t k = 0; k < counts[f]; ++k, ++c)
                out.perCorner[c] = scope == "vtx" || scope == "var" ? items[corners[c]]
                                 : scope == "uni" ? items[f] : items[c];
        return true;
    }

    /* The UV parameters of a mesh: .geom/uv, then the 2D vector parameters
       of .arbGeomParams (where Blender writes its other UV maps) */
    std::vector<UVParam> read_uv_params(const Property &geom, const std::string &path,
                                        const std::vector<uint32_t> &counts,
                                        const std::vector<uint32_t> &corners, size_t nVertices) {
        std::vector<UVParam> params;
        Property uv, arb;
        if (mAr.find(geom, "uv", uv)) {
            UVParam prm;
            prm.key = "";
            prm.name = meta_of(uv, uv, "sourceName");
            if (prm.name.empty())
                prm.name = "uv";
            if (read_uv_param(uv, path, counts, corners, nVertices, prm))
                params.push_back(std::move(prm));
        }
        if (mAr.find(geom, ".arbGeomParams", arb) && arb.type == Property::Compound) {
            for (const Property &a : mAr.properties(arb)) {
                const std::string interpretation = meta_of(a, a, "interpretation");
                if (!interpretation.empty() && interpretation != "vector" && interpretation != "uv")
                    continue;
                UVParam prm;
                prm.key = prm.name = a.name;
                if (read_uv_param(a, path, counts, corners, nVertices, prm))
                    params.push_back(std::move(prm));
            }
        }
        return params;
    }

    bool selected(const std::string &path) const {
        if (mExact)
            return path == mFilter;
        return mFilter.empty() || path == mFilter ||
               (path.size() > mFilter.size() && path.compare(0, mFilter.size(), mFilter) == 0 &&
                path[mFilter.size()] == '/');
    }

    /* 'path' is the filter itself or one of its ancestors */
    bool on_path(const std::string &path) const {
        return path == mFilter ||
               (mFilter.size() > path.size() && mFilter.compare(0, path.size(), path) == 0 &&
                mFilter[path.size()] == '/');
    }

    void visit(const Object &object, const std::string &path,
               const Eigen::Matrix4d &parent, uint32_t depth, bool viaInstance) {
        if (depth > MAX_OBJECT_DEPTH)
            mAr.fail("objects nested deeper than " + std::to_string(MAX_OBJECT_DEPTH) +
                     " levels (or cyclic instances)");
        if (++mVisits > MAX_VISITS)
            mAr.fail("more than " + std::to_string(MAX_VISITS) + " objects (instancing loop?)");

        Eigen::Matrix4d world = parent;
        const std::string schema = object.schema();
        if (schema == SCHEMA_XFORM) {
            bool inherits;
            const Eigen::Matrix4d local = local_transform(mAr, object, inherits);
            world = inherits ? Eigen::Matrix4d(parent * local) : local;
        } else if (schema == SCHEMA_POLYMESH) {
            mGroupUses[object.group]++;
            if (selected(path))
                read_mesh(object, path, world, viaInstance);
        } else if (schema == SCHEMA_SUBD) {
            if (selected(path)) {
                cout << "Skipping subdivision surface \"" << path << "\" (only polygon meshes are supported)" << endl;
                ++skippedSubD;
            }
        }

        for (const Object &child : mAr.children(object)) {
            const std::string childPath = (path == "/" ? "" : path) + "/" + child.name;
            /* Loading one mesh: only its ancestors are walked */
            if (mExact && !on_path(childPath))
                continue;
            auto it = child.meta.find("isInstance");
            if (it != child.meta.end() && it->second == "1") {
                /* Instance: the object at ".instanceSource" appears here */
                Property source;
                if (!mAr.find(mAr.properties(child), ".instanceSource", source))
                    mAr.fail("instance \"" + childPath + "\" has no source");
                Object target;
                const std::string targetPath = mAr.sample_string(source);
                if (!mAr.resolve(targetPath, target))
                    mAr.fail("instance \"" + childPath + "\" refers to missing object \"" + targetPath + "\"");
                visit(target, childPath, world, depth + 1, true);
            } else {
                visit(child, childPath, world, depth + 1, viaInstance);
            }
        }
    }

    void read_mesh(const Object &object, const std::string &path, const Eigen::Matrix4d &world,
                   bool viaInstance) {
        if (meshes.size() >= MAX_MESHES)
            mAr.fail("more than " + std::to_string(MAX_MESHES) + " meshes (instancing loop?)");

        Property geom, P, faceIndices, faceCounts;
        if (!mAr.find(mAr.properties(object), ".geom", geom) || geom.type != Property::Compound)
            mAr.fail("mesh \"" + path + "\" has no geometry");
        if (!mAr.find(geom, "P", P) || !mAr.find(geom, ".faceIndices", faceIndices) ||
            !mAr.find(geom, ".faceCounts", faceCounts))
            mAr.fail("mesh \"" + path + "\" lacks positions or faces");
        if ((P.pod != PodFloat32 && P.pod != PodFloat64) || P.extent != 3 || P.type != Property::Array)
            mAr.fail("mesh \"" + path + "\" has positions of an unsupported type");

        if (!mGeometry) {
            /* Listing: the counts come from the stored sizes, the geometry
               is not read (it is checked when the mesh is loaded) */
            MeshSummary summary;
            summary.path = path;
            summary.vertices = mAr.sample_count(P);
            summary.faces = mAr.sample_count(faceCounts);
            summary.animated = P.samples > 1 || faceIndices.samples > 1 || faceCounts.samples > 1;
            summary.instanced = viaInstance;
            summary.world = world;
            meshes.push_back(summary);
            mGroups.push_back(object.group);
            return;
        }

        std::vector<double> p = mAr.sample_doubles(P);
        std::vector<uint32_t> counts = mAr.sample_indices(faceCounts);
        std::vector<uint32_t> corners = mAr.sample_indices(faceIndices);
        const size_t nVertices = p.size() / 3;

        uint64_t total = 0;
        for (uint32_t c : counts)
            total += c;
        if (total != corners.size())
            mAr.fail("mesh \"" + path + "\": face counts do not match the face indices");
        for (uint32_t index : corners)
            if (index >= nVertices)
                mAr.fail("mesh \"" + path + "\": vertex index " + std::to_string(index) + " out of range");

        MeshSummary summary;
        summary.path = path;
        summary.vertices = nVertices;
        summary.faces = counts.size();
        summary.animated = P.samples > 1 || faceIndices.samples > 1 || faceCounts.samples > 1;
        summary.instanced = viaInstance;
        summary.world = world;
        meshes.push_back(summary);
        mGroups.push_back(object.group);

        const uint64_t base = positions.size();
        if (base + nVertices > 0x7fffffffULL || indices.size() + corners.size() > 0x7fffffffULL)
            mAr.fail("too much geometry (more than 2^31 vertices or face corners)");

        positions.reserve(base + nVertices);
        for (size_t i = 0; i < nVertices; ++i) {
            const Eigen::Vector4d q = world * Eigen::Vector4d(p[3 * i], p[3 * i + 1], p[3 * i + 2], 1.0);
            positions.push_back(Vector3f((Float) q.x(), (Float) q.y(), (Float) q.z()));
        }

        /* UV sets of this mesh, appended to the collected ones */
        std::vector<std::pair<UVAccum *, const UVParam *>> uvTargets;
        std::vector<uint32_t> uvBase;
        std::vector<UVParam> params;
        if (mWantUVs)
            params = read_uv_params(geom, path, counts, corners, nVertices);
        for (const UVParam &prm : params) {
            UVAccum *acc = nullptr;
            for (UVAccum &a : mUVs)
                if (a.key == prm.key)
                    acc = &a;
            if (!acc) {
                mUVs.push_back(UVAccum());
                acc = &mUVs.back();
                acc->key = prm.key;
                acc->name = prm.name;
            }
            if (acc->meshes != meshes.size() - 1)
                continue;   /* missing on an earlier mesh: dropped anyway */
            acc->meshes++;
            uvBase.push_back((uint32_t) (acc->values.size() / 2));
            for (double v : prm.values)
                acc->values.push_back((float) v);
            uvTargets.emplace_back(acc, &prm);
        }

        /* Alembic polygons are clockwise: reverse every face to get the
           counter-clockwise order of OBJ files */
        size_t offset = 0;
        for (uint32_t count : counts) {
            if (count < 3) {
                ++skippedFaces;
            } else {
                for (uint32_t k = 0; k < count; ++k) {
                    indices.push_back((uint32_t) (base + corners[offset + count - 1 - k]));
                    for (size_t t = 0; t < uvTargets.size(); ++t)
                        uvTargets[t].first->corners.push_back(
                            uvBase[t] + uvTargets[t].second->perCorner[offset + count - 1 - k]);
                }
                sizes.push_back(count);
            }
            offset += count;
        }
    }

    Archive &mAr;
    std::string mFilter;
    bool mGeometry, mExact, mWantUVs;
    std::deque<UVAccum> mUVs;     /* deque: pointers to elements stay valid */
    uint64_t mVisits = 0;
    std::vector<uint64_t> mGroups;                 /* object group of each mesh */
    std::map<uint64_t, uint32_t> mGroupUses;       /* visits per mesh object group */
};

} // namespace

std::vector<MeshSummary> list_meshes(const std::string &filename) {
    Archive ar(filename);
    MeshCollector collector(ar, "", false);
    collector.run();
    return collector.meshes;
}

void load_abc_mesh(const std::string &filename, const std::string &path,
                   MatrixXu &F, MatrixXf &V, uint64_t *polygons, std::vector<UVSet> *uvs) {
    Archive ar(filename);
    MeshCollector collector(ar, path, true, true, uvs != nullptr);
    collector.run();
    if (collector.meshes.size() != 1)
        ar.fail(collector.meshes.empty() ? "no polygon mesh at \"" + path + "\""
                                         : "\"" + path + "\" is instanced");
    if (uvs)
        *uvs = collector.take_uvs(filename + ":" + path);
    build_mesh(collector.positions, collector.sizes, collector.indices, F, V, filename + ":" + path, uvs);
    if (polygons)
        *polygons = collector.sizes.size();
}

bool glob_match(const std::string &pattern, const std::string &text) {
    /* Iterative matcher with single-star backtracking: linear memory,
       O(pattern x text) time, no recursion */
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*')
        ++p;
    return p == pattern.size();
}

void load_abc(const std::string &filename, MatrixXu &F, MatrixXf &V,
              const std::string &object, const ProgressCallback &progress,
              uint64_t *polygons, std::vector<UVSet> *uvs) {
    cout << "Loading \"" << filename << "\" .. ";
    cout.flush();
    Timer<> timer;

    Archive ar(filename);
    MeshCollector collector(ar, object, true, false, uvs != nullptr);
    collector.run();

    if (collector.meshes.empty()) {
        if (!object.empty())
            ar.fail("no polygon mesh at \"" + object + "\"");
        ar.fail(collector.skippedSubD > 0
                    ? "contains only subdivision surfaces, no polygon mesh"
                    : "contains no polygon mesh");
    }

    if (uvs)
        *uvs = collector.take_uvs(filename);
    build_mesh(collector.positions, collector.sizes, collector.indices, F, V, filename, uvs);
    if (polygons)
        *polygons = collector.sizes.size();

    cout << "done. (" << collector.meshes.size() << " mesh"
         << (collector.meshes.size() > 1 ? "es" : "") << ", V=" << V.cols()
         << ", F=" << F.cols() << ", took " << timeString(timer.value()) << ")" << endl;
    if (collector.skippedFaces > 0)
        cout << "Warning: skipped " << collector.skippedFaces
             << " degenerate faces with fewer than 3 vertices" << endl;
}

} // namespace abc
