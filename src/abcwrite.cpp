/*
    abcwrite.cpp: Alembic writer and hash verifier (see abc.h / abchash.h)

    Hash chain of the Alembic library, reproduced byte for byte:

        sample key     = MurmurHash3(values)
        sample digest  = key, or for arrays Spooky(dimensions, key)
        property hash  = Spooky(header bytes, digest of sample 0 combined
                         with the following samples through ShortEnd)
        compound hash  = Spooky(sub-property hashes, name + metadata)
        object data    = Spooky(hashes of its top-level properties)
        object hash    = Spooky(child object hashes) -> "children" hash, then
                         continued with data hash, metadata and name
    Every object stores [data hash, children hash] (32 bytes) after the
    headers of its children.
*/

#include "abc.h"
#include "abchash.h"
#include "meshio.h"
#include <array>
#include <cfloat>
#include <set>

namespace abc {

using ogawa::is_data;

namespace {

/* Writes a uint32 on 1, 2 or 4 bytes (little endian) */
void push_hinted(std::vector<uint8_t> &out, uint32_t value, uint32_t hint) {
    const uint32_t bytes = hint == 0 ? 1 : (hint == 1 ? 2 : 4);
    for (uint32_t i = 0; i < bytes; ++i)
        out.push_back((uint8_t) (value >> (8 * i)));
}

template <typename T> void push_raw(std::vector<uint8_t> &out, const T &value) {
    const uint8_t *p = (const uint8_t *) &value;
    out.insert(out.end(), p, p + sizeof(T));
}

/* Bytes hashed for a property header (HashPropertyHeader) */
std::vector<uint8_t> header_hash_input(const std::string &name, const MetaData &meta,
                                       Property::Type type, Pod pod, uint32_t extent,
                                       const TimeSampling &ts) {
    std::vector<uint8_t> d(name.begin(), name.end());
    const std::string m = serialize(meta);
    d.insert(d.end(), m.begin(), m.end());
    if (type != Property::Compound) {
        d.push_back((uint8_t) pod);
        d.push_back((uint8_t) extent);
        if (type == Property::Scalar)
            d.push_back(0);
        push_raw(d, ts.timePerCycle);
        push_raw(d, (uint32_t) ts.times.size());
        for (double t : ts.times)
            push_raw(d, t);
    }
    return d;
}

Digest hash_dimensions(const std::vector<uint64_t> &dims, const Digest &key) {
    if (dims.empty())
        return key;
    SpookyHash h;
    h.init(0, 0);
    h.update(dims.data(), dims.size() * 8);
    h.update(key.words, 16);
    return h.final();
}

Digest property_hash(const std::vector<uint8_t> &headerInput,
                     const std::vector<Digest> &sampleDigests) {
    SpookyHash h;
    h.init(0, 0);
    if (!headerInput.empty())
        h.update(headerInput.data(), headerInput.size());
    if (!sampleDigests.empty()) {
        Digest acc = sampleDigests[0];
        for (size_t i = 1; i < sampleDigests.size(); ++i) {
            Digest d = sampleDigests[i];
            SpookyHash::short_end(acc.words[0], acc.words[1], d.words[0], d.words[1]);
        }
        h.update(acc.words, 16);
    }
    return h.final();
}

/* Finishes an object hash: 'children' already holds the child hashes (if
   any, final() was called on it to produce the stored children hash) */
Digest object_hash(SpookyHash &children, const Digest &data, const MetaData &meta,
                   const std::string &name) {
    children.update(data.words, 16);
    const std::string m = serialize(meta);
    if (!m.empty())
        children.update(m.data(), m.size());
    children.update(name.data(), name.size());
    return children.final();
}

/* ------------------------------------------------------------------------- */
/*  Verification                                                             */
/* ------------------------------------------------------------------------- */

/* Recomputes hashes from the file content */
class HashCalc {
public:
    explicit HashCalc(Archive &ar) : mAr(ar), mIn(ar.reader()) { }

    /* Verifies the whole archive, returns the number of objects checked */
    uint64_t verify() {
        object(mAr.top());
        return mObjects;
    }

    /* Hash an object passes to its parent, from the hashes stored in the
       file (no sample data is read): used for untouched objects */
    Digest stored_object_hash(const Object &o) {
        SpookyHash h;
        h.init(0, 0);
        std::vector<Object> children = mAr.children(o);
        if (!children.empty()) {
            for (const Object &child : children) {
                const Digest d = stored_object_hash(child);
                h.update(d.words, 16);
            }
            uint64_t unused[2];
            h.final(&unused[0], &unused[1]);
        }
        return object_hash(h, stored_hashes(o)[0], o.meta, o.name);
    }

    /* [data hash, children hash] stored after an object's child headers */
    std::array<Digest, 2> stored_hashes(const Object &o) {
        std::vector<uint64_t> g = mIn.group(o.group);
        if (g.empty() || !is_data(g.back()) || mIn.data_size(g.back()) < 32)
            mAr.fail("object \"" + o.path + "\" has no stored hashes");
        std::array<Digest, 2> stored;
        mIn.read_data(g.back(), mIn.data_size(g.back()) - 32, 32, stored.data());
        return stored;
    }

    Digest property(const Property &p) {
        if (p.type == Property::Compound) {
            SpookyHash h;
            h.init(0, 0);
            for (const Property &child : mAr.properties(p)) {
                const Digest d = property(child);
                h.update(d.words, 16);
            }
            std::vector<uint8_t> in = header_hash_input(p.name, p.meta, p.type, p.pod, p.extent,
                                                        mAr.time_samplings()[0]);
            if (!in.empty())
                h.update(in.data(), in.size());
            return h.final();
        }

        std::vector<uint64_t> g = mIn.group(p.group);
        std::vector<Digest> digests;
        for (uint32_t i = 0; i < p.samples; ++i) {
            const size_t stored = mAr.stored_index(p, i);
            const size_t index = p.type == Property::Array ? 2 * stored : stored;
            if (index >= g.size() || !is_data(g[index]))
                mAr.fail("property \"" + p.name + "\" misses sample " + std::to_string(i));

            /* Stored key, checked against the values */
            Digest key;
            const uint64_t size = mIn.data_size(g[index]);
            uint64_t payload = 0;
            if (size > 0) {
                if (size < 16)
                    mAr.fail("truncated sample in property \"" + p.name + "\"");
                payload = size - 16;
                mIn.read_data(g[index], 0, 16, key.words);
                std::vector<uint8_t> values((size_t) payload);
                if (payload > 0)
                    mIn.read_data(g[index], 16, payload, values.data());
                if (murmur3(values.data(), values.size()) != key)
                    mAr.fail("sample key mismatch in property \"" + p.name + "\"");
            }

            if (p.type == Property::Array) {
                if (index + 1 >= g.size() || !is_data(g[index + 1]))
                    mAr.fail("property \"" + p.name + "\" misses its dimensions");
                std::vector<uint64_t> dims;
                const uint64_t dimSize = mIn.data_size(g[index + 1]);
                if (dimSize > 0) {
                    if (dimSize % 8 != 0)
                        mAr.fail("invalid dimensions in property \"" + p.name + "\"");
                    dims.resize((size_t) (dimSize / 8));
                    mIn.read_data(g[index + 1], 0, dimSize, dims.data());
                } else {
                    const uint64_t element = (uint64_t) pod_size(p.pod) * p.extent;
                    dims.push_back(element > 0 ? payload / element : 0);
                }
                key = hash_dimensions(dims, key);
            }
            digests.push_back(key);
        }
        return property_hash(header_hash_input(p.name, p.meta, p.type, p.pod, p.extent,
                                               mAr.time_samplings()[p.timeSampling]),
                             digests);
    }

private:
    Digest object(const Object &o) {
        SpookyHash data;
        data.init(0, 0);
        for (const Property &p : mAr.properties(mAr.properties(o))) {
            const Digest d = property(p);
            data.update(d.words, 16);
        }
        const Digest dataHash = data.final();

        SpookyHash h;
        h.init(0, 0);
        Digest childrenHash;
        std::vector<Object> children = mAr.children(o);
        if (!children.empty()) {
            for (const Object &child : children) {
                const Digest d = object(child);
                h.update(d.words, 16);
            }
            h.final(&childrenHash.words[0], &childrenHash.words[1]);
        }

        const std::array<Digest, 2> stored = stored_hashes(o);
        if (stored[0] != dataHash)
            mAr.fail("property hash mismatch on object \"" + o.path + "\"");
        if (stored[1] != childrenHash)
            mAr.fail("children hash mismatch on object \"" + o.path + "\"");
        ++mObjects;

        return object_hash(h, dataHash, o.meta, o.name);
    }

    Archive &mAr;
    ogawa::Reader &mIn;
    uint64_t mObjects = 0;
};

/* ------------------------------------------------------------------------- */
/*  Writing                                                                  */
/* ------------------------------------------------------------------------- */

/* In-memory description of what to write (one sample per property) */
struct OutProperty {
    std::string name;
    Property::Type type = Property::Compound;
    Pod pod = PodUint8;
    uint32_t extent = 1;
    MetaData meta;
    std::vector<uint8_t> values;          ///< scalar / array: the sample
    std::vector<OutProperty> children;    ///< compound: sub-properties
};

struct OutObject {
    std::string name;
    MetaData meta;
    std::vector<OutProperty> properties;
    std::vector<OutObject> children;
};

template <typename T>
OutProperty make_value(const std::string &name, Property::Type type, Pod pod, uint32_t extent,
                       const T *values, size_t count, const MetaData &meta = MetaData()) {
    OutProperty p;
    p.name = name;
    p.type = type;
    p.pod = pod;
    p.extent = extent;
    p.meta = meta;
    p.values.resize(count * sizeof(T));
    if (count > 0)
        memcpy(p.values.data(), values, count * sizeof(T));
    return p;
}

OutProperty make_compound(const std::string &name, const MetaData &meta,
                          const std::vector<OutProperty> &children) {
    OutProperty p;
    p.name = name;
    p.meta = meta;
    p.children = children;
    return p;
}

class ArchiveWriter {
public:
    /* 'table': indexed metadata to start from (when copying an archive, so
       that the copied headers keep their indices) */
    explicit ArchiveWriter(ogawa::Writer &out,
                           const std::vector<std::string> &table = std::vector<std::string>())
        : mOut(out), mMetaTable(table) {
        for (size_t i = 0; i < table.size(); ++i)
            mMetaIndex.insert(std::make_pair(table[i], (uint32_t) i));
    }

    void write(const OutObject &top, const MetaData &archiveMeta) {
        const int32_t formatVersion = 0, libraryVersion = 10803;
        const uint64_t v0 = mOut.add_data(&formatVersion, 4);
        const uint64_t v1 = mOut.add_data(&libraryVersion, 4);

        Digest hash;
        const uint64_t topGroup = write_object(top, hash, nullptr);

        const std::string meta = serialize(archiveMeta);
        const uint64_t metaData = mOut.add_data(meta.data(), meta.size());

        /* Single default time sampling: 1 sample at t = 0 */
        std::vector<uint8_t> ts;
        push_raw(ts, (uint32_t) 1);
        push_raw(ts, 1.0);
        push_raw(ts, (uint32_t) 1);
        push_raw(ts, 0.0);
        const uint64_t timeSamplings = mOut.add_data(ts.data(), ts.size());

        const uint64_t indexed = metadata_table();

        mOut.commit(mOut.add_group({ v0, v1, topGroup, metaData, timeSamplings, indexed }));
    }

    /* Writes the indexed metadata table, returns its entry */
    uint64_t metadata_table() {
        std::vector<uint8_t> table;
        for (const std::string &s : mMetaTable) {
            table.push_back((uint8_t) s.size());
            table.insert(table.end(), s.begin(), s.end());
        }
        return mOut.add_data(table.data(), table.size());
    }

    /* MetaDataMap::getIndex: 0 = empty, 1..254 = table, 0xff = inline */
    uint32_t meta_index(const std::string &s) {
        if (s.empty())
            return 0;
        if (s.size() < 256) {
            auto it = mMetaIndex.find(s);
            if (it != mMetaIndex.end())
                return it->second + 1;
            if (mMetaIndex.size() < 254) {
                const uint32_t index = (uint32_t) mMetaTable.size();
                mMetaIndex[s] = index;
                mMetaTable.push_back(s);
                return index + 1;
            }
        }
        return 0xff;
    }

    /* WritePropertyInfo for a property with one constant sample */
    void property_header(const OutProperty &p, uint32_t samples, std::vector<uint8_t> &out) {
        const std::string meta = serialize(p.meta);
        const uint32_t maxSize = std::max<uint32_t>(
            std::max<uint32_t>((uint32_t) meta.size(), (uint32_t) p.name.size()), samples);
        const uint32_t hint = maxSize > 255 && maxSize < 65536 ? 1 : (maxSize >= 65536 ? 2 : 0);
        const uint32_t metaIndex = meta_index(meta);

        uint32_t info = (hint << 2) & 0xc;
        info |= (metaIndex << 20) & 0xff00000;
        if (p.type != Property::Compound) {
            const bool scalarLike = p.type == Property::Scalar || element_count(p) == 1;
            info |= (p.type == Property::Scalar ? 1 : 2) & 0x3;
            info |= scalarLike ? 1 : 0;
            info |= ((uint32_t) p.pod << 4) & 0xf0;
            info |= 0x800;                     /* first = last changed = 0 */
            info |= (p.extent << 12) & 0xff000;
            info |= 0x400;                     /* homogenous */
            push_hinted(out, info, 2);
            push_hinted(out, samples, hint);
        } else {
            push_hinted(out, info, 2);
        }
        push_hinted(out, (uint32_t) p.name.size(), hint);
        out.insert(out.end(), p.name.begin(), p.name.end());
        if (metaIndex == 0xff) {
            push_hinted(out, (uint32_t) meta.size(), hint);
            out.insert(out.end(), meta.begin(), meta.end());
        }
    }

    static uint64_t element_count(const OutProperty &p) {
        const uint64_t element = (uint64_t) pod_size(p.pod) * p.extent;
        return element > 0 ? p.values.size() / element : 0;
    }

public:
    /* Writes a property group, appends its header, returns its entry */
    uint64_t write_property(const OutProperty &p, std::vector<uint8_t> &headers, Digest &hash) {
        const TimeSampling ts { 1.0, { 0.0 } };
        if (p.type == Property::Compound) {
            std::vector<uint64_t> entries;
            std::vector<uint8_t> subHeaders;
            SpookyHash h;
            h.init(0, 0);
            for (const OutProperty &child : p.children) {
                Digest d;
                entries.push_back(write_property(child, subHeaders, d));
                h.update(d.words, 16);
            }
            if (!subHeaders.empty())
                entries.push_back(mOut.add_data(subHeaders.data(), subHeaders.size()));
            std::vector<uint8_t> in = header_hash_input(p.name, p.meta, p.type, p.pod, p.extent, ts);
            if (!in.empty())
                h.update(in.data(), in.size());
            hash = h.final();
            property_header(p, 0, headers);
            return mOut.add_group(entries);
        }

        if (pod_size(p.pod) == 0 || p.values.empty())
            throw std::runtime_error("Alembic writer: unsupported empty or string property \"" + p.name + "\"!");
        const uint64_t element = (uint64_t) pod_size(p.pod) * p.extent;
        if (p.values.size() % element != 0 ||
            (p.type == Property::Scalar && p.values.size() != element))
            throw std::runtime_error("Alembic writer: invalid size for property \"" + p.name + "\"!");

        /* Sample data: 16-byte key followed by the values */
        Digest key = murmur3(p.values.data(), p.values.size());
        std::vector<uint8_t> blob((const uint8_t *) key.words, (const uint8_t *) key.words + 16);
        blob.insert(blob.end(), p.values.begin(), p.values.end());
        std::vector<uint64_t> entries { mOut.add_data(blob.data(), blob.size()) };

        if (p.type == Property::Array) {
            entries.push_back(ogawa::EMPTY_DATA);   /* rank 1: dimensions implied */
            key = hash_dimensions({ element_count(p) }, key);
        }
        hash = property_hash(header_hash_input(p.name, p.meta, p.type, p.pod, p.extent, ts), { key });
        property_header(p, 1, headers);
        return mOut.add_group(entries);
    }

private:
    /* Writes an object group; appends its header to 'parentHeaders' (if
       any) and returns its entry together with the hash for its parent */
    uint64_t write_object(const OutObject &o, Digest &hash, std::vector<uint8_t> *parentHeaders) {
        /* Top-level properties: no header of their own */
        std::vector<uint64_t> propEntries;
        std::vector<uint8_t> propHeaders;
        SpookyHash data;
        data.init(0, 0);
        for (const OutProperty &p : o.properties) {
            Digest d;
            propEntries.push_back(write_property(p, propHeaders, d));
            data.update(d.words, 16);
        }
        if (!propHeaders.empty())
            propEntries.push_back(mOut.add_data(propHeaders.data(), propHeaders.size()));
        const uint64_t propGroup = mOut.add_group(propEntries);
        const Digest dataHash = data.final();

        std::vector<uint64_t> entries { propGroup };
        std::vector<uint8_t> childHeaders;
        SpookyHash h;
        h.init(0, 0);
        Digest childrenHash;
        if (!o.children.empty()) {
            for (const OutObject &child : o.children) {
                Digest d;
                entries.push_back(write_object(child, d, &childHeaders));
                h.update(d.words, 16);
            }
            h.final(&childrenHash.words[0], &childrenHash.words[1]);
        }
        childHeaders.insert(childHeaders.end(), (const uint8_t *) dataHash.words,
                            (const uint8_t *) dataHash.words + 16);
        childHeaders.insert(childHeaders.end(), (const uint8_t *) childrenHash.words,
                            (const uint8_t *) childrenHash.words + 16);
        entries.push_back(mOut.add_data(childHeaders.data(), childHeaders.size()));

        hash = object_hash(h, dataHash, o.meta, o.name);

        if (parentHeaders) {   /* WriteObjectHeader */
            const std::string meta = serialize(o.meta);
            const uint32_t metaIndex = meta_index(meta);
            push_hinted(*parentHeaders, (uint32_t) o.name.size(), 2);
            parentHeaders->insert(parentHeaders->end(), o.name.begin(), o.name.end());
            push_hinted(*parentHeaders, metaIndex, 0);
            if (metaIndex == 0xff) {
                push_hinted(*parentHeaders, (uint32_t) meta.size(), 2);
                parentHeaders->insert(parentHeaders->end(), meta.begin(), meta.end());
            }
        }
        return mOut.add_group(entries);
    }

    ogawa::Writer &mOut;
    std::map<std::string, uint32_t> mMetaIndex;
    std::vector<std::string> mMetaTable;
};

/* Geometry of a PolyMesh ready to be written */
struct MeshData {
    std::vector<float> P;
    std::vector<int32_t> faceIndices, faceCounts;
    double bounds[6] = { DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
    size_t irregular = 0;

    std::vector<OutProperty> geom_properties() const {
        return {
            make_value(".selfBnds", Property::Scalar, PodFloat64, 6, bounds, 6, {{ "interpretation", "box" }}),
            make_value("P", Property::Array, PodFloat32, 3, P.data(), P.size(),
                       {{ "geoScope", "vtx" }, { "interpretation", "point" }}),
            make_value(".faceIndices", Property::Array, PodInt32, 1, faceIndices.data(), faceIndices.size()),
            make_value(".faceCounts", Property::Array, PodInt32, 1, faceCounts.data(), faceCounts.size())
        };
    }
};

/* Polygons of an extracted mesh, positions mapped by 'transform', faces
   clockwise. Only the vertices used by polygons are kept, in their
   original order: quad-dominant extraction leaves the centers of irregular
   faces unused (Blender's OBJ importer drops them too). */
MeshData make_mesh_data(const MatrixXu &F, const MatrixXf &V, const Eigen::Matrix4d &transform) {
    MeshData m;
    std::vector<uint32_t> sizes, ccw, faceIds;
    m.irregular = extracted_polygons(F, sizes, ccw, faceIds);
    if (sizes.empty() || V.cols() == 0)
        throw std::runtime_error("Alembic writer: the mesh is empty!");
    if (V.cols() > 0x7fffffff || ccw.size() > 0x7fffffff)
        throw std::runtime_error("Alembic writer: the mesh is too large!");

    std::vector<int32_t> remap((size_t) V.cols(), -1);
    for (uint32_t index : ccw) {
        if (index >= V.cols())
            throw std::runtime_error("Alembic writer: vertex index out of range!");
        remap[index] = 0;
    }
    int32_t used = 0;
    for (uint32_t i = 0; i < V.cols(); ++i) {
        if (remap[i] < 0)
            continue;
        remap[i] = used++;
        const Eigen::Vector4d q = transform * Eigen::Vector4d(V(0, i), V(1, i), V(2, i), 1.0);
        for (int k = 0; k < 3; ++k) {
            const float x = (float) q[k];
            if (!std::isfinite(x))
                throw std::runtime_error("Alembic writer: invalid vertex position!");
            m.P.push_back(x);
            m.bounds[k] = std::min(m.bounds[k], (double) x);
            m.bounds[k + 3] = std::max(m.bounds[k + 3], (double) x);
        }
    }

    m.faceIndices.resize(ccw.size());
    m.faceCounts.resize(sizes.size());
    size_t offset = 0;
    for (size_t f = 0; f < sizes.size(); ++f) {
        m.faceCounts[f] = (int32_t) sizes[f];
        for (uint32_t k = 0; k < sizes[f]; ++k)
            m.faceIndices[offset + k] = remap[ccw[offset + sizes[f] - 1 - k]];
        offset += sizes[f];
    }
    return m;
}

/* Object name from the output file name ("C:/x/scene_retopo.abc" ->
   "scene_retopo"); characters that Alembic paths cannot hold are replaced */
std::string object_name(const std::string &filename) {
    size_t start = filename.find_last_of("/\\");
    start = start == std::string::npos ? 0 : start + 1;
    size_t end = filename.rfind('.');
    if (end == std::string::npos || end < start)
        end = filename.size();
    std::string name = filename.substr(start, end - start);
    for (char &c : name)
        if (c == '/' || c == '\\' || c == ';' || c == '=' || (unsigned char) c < 32)
            c = '_';
    return name.empty() ? "InstantMeshes" : name;
}


/* ------------------------------------------------------------------------- */
/*  Splicing: copy an archive, replacing the geometry of some meshes         */
/* ------------------------------------------------------------------------- */

const char *SCHEMA_FACESET = "AbcGeom_FaceSet_v1";

class Splicer {
public:
    struct Target {
        MeshData data;
        std::string path;
    };

    Splicer(Archive &ar, ogawa::Writer &out, const std::map<std::string, Target> &targets)
        : mAr(ar), mIn(ar.reader()), mOut(out), mCopier(ar.reader(), out),
          mWriter(out, ar.indexed_metadata()), mCalc(ar), mTargets(targets) {
        /* Objects to rebuild: the targets and all their ancestors */
        for (const auto &t : targets) {
            std::string path = t.first;
            while (!path.empty() && path != "/") {
                mDirty.insert(path);
                path = path.substr(0, path.rfind('/'));
            }
        }
    }

    /* Writes everything, returns the new root group entry (not committed) */
    uint64_t run() {
        std::vector<uint64_t> root = mIn.group(mIn.root());
        std::vector<uint64_t> entries;
        entries.push_back(mCopier.copy(root[0]));    /* format version */
        entries.push_back(mCopier.copy(root[1]));    /* library version */
        Digest unused;
        entries.push_back(rebuild_object(mAr.top(), "/", unused));
        entries.push_back(mCopier.copy(root[3]));    /* archive metadata */
        entries.push_back(mCopier.copy(root[4]));    /* time samplings */
        entries.push_back(mWriter.metadata_table()); /* original table + additions */
        for (size_t i = 6; i < root.size(); ++i)     /* unknown extensions */
            entries.push_back(mCopier.copy(root[i]));
        for (const auto &t : mTargets)
            if (!mDone.count(t.first))
                mAr.fail("mesh \"" + t.first + "\" not found while copying");
        return mOut.add_group(entries);
    }

    std::vector<std::string> notes;

private:
    struct Built {
        uint64_t entry = ogawa::EMPTY_GROUP;
        Digest hash;
        std::vector<uint8_t> header;
    };

    Built copy_property(const Property &p) {
        Built b;
        b.entry = mCopier.copy(p.group);
        b.hash = mCalc.property(p);
        b.header = p.rawHeader;
        return b;
    }

    Built new_property(const OutProperty &p) {
        Built b;
        b.entry = mWriter.write_property(p, b.header, b.hash);
        return b;
    }

    /* Group of sub-properties followed by their headers */
    uint64_t property_group(const std::vector<Built> &children) {
        std::vector<uint64_t> entries;
        std::vector<uint8_t> headers;
        for (const Built &b : children) {
            entries.push_back(b.entry);
            headers.insert(headers.end(), b.header.begin(), b.header.end());
        }
        if (!headers.empty())
            entries.push_back(mOut.add_data(headers.data(), headers.size()));
        return mOut.add_group(entries);
    }

    /* A compound whose own header is unchanged but whose content is new */
    Built compound(const Property &original, const std::vector<Built> &children) {
        Built b;
        b.entry = property_group(children);
        SpookyHash h;
        h.init(0, 0);
        for (const Built &c : children)
            h.update(c.hash.words, 16);
        std::vector<uint8_t> in = header_hash_input(original.name, original.meta, Property::Compound,
                                                    original.pod, original.extent, TimeSampling());
        if (!in.empty())
            h.update(in.data(), in.size());
        b.hash = h.final();
        b.header = original.rawHeader;
        return b;
    }

    static bool per_element(const Property &p) {
        auto it = p.meta.find("geoScope");
        return it != p.meta.end() && (it->second == "vtx" || it->second == "fvr" ||
                                      it->second == "uni" || it->second == "var");
    }

    /* New .geom: new positions and faces, per-element data dropped */
    Built rebuild_geom(const Property &geom, const Target &target) {
        std::vector<Built> children;
        for (const OutProperty &p : target.data.geom_properties())
            children.push_back(new_property(p));

        static const std::set<std::string> replaced { ".selfBnds", "P", ".faceIndices", ".faceCounts" };
        std::vector<std::string> dropped;
        for (const Property &p : mAr.properties(geom)) {
            if (replaced.count(p.name))
                continue;
            if (p.name == ".arbGeomParams" && p.type == Property::Compound) {
                std::vector<Built> kept;
                for (const Property &a : mAr.properties(p)) {
                    if (per_element(a))
                        dropped.push_back(a.name);
                    else
                        kept.push_back(copy_property(a));
                }
                children.push_back(compound(p, kept));
            } else if (p.name == "N" || p.name == "uv" || p.name == ".velocities" || per_element(p)) {
                dropped.push_back(p.name);
            } else {
                children.push_back(copy_property(p));
            }
        }
        if (!dropped.empty()) {
            std::string list;
            for (const std::string &d : dropped)
                list += (list.empty() ? "" : ", ") + d;
            notes.push_back(target.path + ": dropped " + list + " (no longer matching the topology)");
        }
        return compound(geom, children);
    }

    /* New .faceset: every face of the new mesh */
    Built rebuild_faceset(const Property &faceset, size_t faces) {
        std::vector<int32_t> all(faces);
        for (size_t i = 0; i < faces; ++i)
            all[i] = (int32_t) i;
        std::vector<Built> children;
        bool found = false;
        for (const Property &p : mAr.properties(faceset)) {
            if (p.name == ".faces") {
                children.push_back(new_property(make_value(".faces", Property::Array, PodInt32, 1,
                                                           all.data(), all.size(), p.meta)));
                found = true;
            } else {
                children.push_back(copy_property(p));
            }
        }
        if (!found)
            mAr.fail("face set without faces");
        return compound(faceset, children);
    }

    enum Mode { Ancestor, Mesh, FaceSet };

    /* Rebuilds an object group, returns its entry; 'hash' receives the hash
       for its parent */
    uint64_t rebuild_object(const Object &o, const std::string &path, Digest &hash,
                            Mode mode = Ancestor, const Target *target = nullptr) {
        std::vector<uint64_t> g = mIn.group(o.group);

        /* Properties */
        uint64_t propGroup;
        Digest dataHash;
        if (mode == Ancestor) {
            propGroup = g.empty() || is_data(g[0]) ? ogawa::EMPTY_GROUP : mCopier.copy(g[0]);
            dataHash = mCalc.stored_hashes(o)[0];
        } else {
            std::vector<Built> props;
            bool rebuilt = false;
            for (const Property &p : mAr.properties(mAr.properties(o))) {
                if (mode == Mesh && p.name == ".geom" && p.type == Property::Compound) {
                    props.push_back(rebuild_geom(p, *target));
                    rebuilt = true;
                } else if (mode == FaceSet && p.name == ".faceset" && p.type == Property::Compound) {
                    props.push_back(rebuild_faceset(p, target->data.faceCounts.size()));
                    rebuilt = true;
                } else {
                    props.push_back(copy_property(p));
                }
            }
            if (!rebuilt)
                mAr.fail("\"" + path + "\" has no geometry to replace");
            propGroup = property_group(props);
            SpookyHash data;
            data.init(0, 0);
            for (const Built &b : props)
                data.update(b.hash.words, 16);
            dataHash = data.final();
        }

        /* Children */
        const std::vector<Object> children = mAr.children(o);
        size_t faceSets = 0;
        if (mode == Mesh)
            for (const Object &c : children)
                faceSets += c.schema() == SCHEMA_FACESET;
        if (faceSets > 1)
            notes.push_back(path + ": dropped " + std::to_string(faceSets) +
                            " face sets (per-face materials cannot follow the new faces)");
        else if (faceSets == 1)
            notes.push_back(path + ": face set rebuilt over all new faces");

        std::vector<uint64_t> entries { propGroup };
        std::vector<uint8_t> headers;
        SpookyHash h;
        h.init(0, 0);
        Digest childrenHash;
        bool any = false;
        for (const Object &c : children) {
            const std::string childPath = (path == "/" ? "" : path) + "/" + c.name;
            Digest d;
            uint64_t entry;
            if (mode == Mesh && c.schema() == SCHEMA_FACESET) {
                if (faceSets > 1)
                    continue;
                entry = rebuild_object(c, childPath, d, FaceSet, target);
            } else if (mode == Ancestor && mTargets.count(childPath)) {
                entry = rebuild_object(c, childPath, d, Mesh, &mTargets.at(childPath));
                mDone.insert(childPath);
            } else if (mode == Ancestor && mDirty.count(childPath)) {
                entry = rebuild_object(c, childPath, d, Ancestor);
            } else {
                entry = mCopier.copy(c.group);
                d = mCalc.stored_object_hash(c);
            }
            entries.push_back(entry);
            headers.insert(headers.end(), c.rawHeader.begin(), c.rawHeader.end());
            h.update(d.words, 16);
            any = true;
        }
        if (any)
            h.final(&childrenHash.words[0], &childrenHash.words[1]);

        headers.insert(headers.end(), (const uint8_t *) dataHash.words,
                       (const uint8_t *) dataHash.words + 16);
        headers.insert(headers.end(), (const uint8_t *) childrenHash.words,
                       (const uint8_t *) childrenHash.words + 16);
        entries.push_back(mOut.add_data(headers.data(), headers.size()));

        hash = object_hash(h, dataHash, o.meta, o.name);
        return mOut.add_group(entries);
    }

    Archive &mAr;
    ogawa::Reader &mIn;
    ogawa::Writer &mOut;
    ogawa::Copier mCopier;
    ArchiveWriter mWriter;
    HashCalc mCalc;
    const std::map<std::string, Target> &mTargets;
    std::set<std::string> mDirty, mDone;
};

} // namespace

uint64_t verify_hashes(const std::string &filename) {
    Archive ar(filename);
    return HashCalc(ar).verify();
}

void write_abc(const std::string &filename, const MatrixXu &F, const MatrixXf &V,
               const ProgressCallback &progress) {
    Timer<> timer;
    cout << "Writing \"" << filename << "\" (V=" << V.cols() << ", F=" << F.cols() << ") .. ";
    cout.flush();

    const MeshData meshData = make_mesh_data(F, V, Eigen::Matrix4d::Identity());

    MetaData polyMeta {{ "schema", "AbcGeom_PolyMesh_v1" }, { "schemaBaseType", "AbcGeom_GeomBase_v1" }};
    MetaData polyObjMeta = polyMeta;
    polyObjMeta["schemaObjTitle"] = "AbcGeom_PolyMesh_v1:.geom";
    MetaData xformMeta {{ "schema", "AbcGeom_Xform_v3" }};
    MetaData xformObjMeta = xformMeta;
    xformObjMeta["schemaObjTitle"] = "AbcGeom_Xform_v3:.xform";

    const std::string name = object_name(filename);

    OutObject mesh;
    mesh.name = name;
    mesh.meta = polyObjMeta;
    mesh.properties.push_back(make_compound(".geom", polyMeta, meshData.geom_properties()));

    const uint8_t inherits = 1, matrixOp = 0x30;
    const double identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
    OutObject xform;
    xform.name = name;
    xform.meta = xformObjMeta;
    xform.properties.push_back(make_compound(".xform", xformMeta, {
        make_value(".inherits", Property::Scalar, PodBool, 1, &inherits, 1),
        make_value(".ops", Property::Scalar, PodUint8, 1, &matrixOp, 1),
        make_value(".vals", Property::Scalar, PodFloat64, 16, identity, 16)
    }));
    xform.children.push_back(mesh);

    OutObject top;
    top.name = "ABC";
    top.children.push_back(xform);

    ogawa::Writer out(filename);
    ArchiveWriter(out).write(top, {{ "_ai_Application", "Instant Meshes" },
                                   { "_ai_AlembicVersion", "Instant Meshes native Ogawa writer" }});

    cout << "done. (";
    if (meshData.irregular > 0)
        cout << meshData.irregular << " irregular faces, ";
    cout << "took " << timeString(timer.value()) << ")" << endl;
}

void splice_abc(const std::string &input, const std::string &output,
                const std::vector<Replacement> &replacements) {
    Timer<> timer;
    cout << "Writing \"" << output << "\" (" << replacements.size() << " mesh"
         << (replacements.size() > 1 ? "es" : "") << " replaced) .. ";
    cout.flush();

    ogawa::Writer out(output);   /* temporary file until commit() */
    uint64_t root;
    std::vector<std::string> notes;
    {
        Archive ar(input);

        /* World transform of every target, checked to be invertible */
        std::map<std::string, MeshSummary> meshes;
        for (const MeshSummary &m : list_meshes(input))
            meshes[m.path] = m;

        std::map<std::string, Splicer::Target> targets;
        for (const Replacement &r : replacements) {
            auto it = meshes.find(r.path);
            if (it == meshes.end())
                ar.fail("no polygon mesh at \"" + r.path + "\"");
            if (it->second.instanced)
                ar.fail("\"" + r.path + "\" is instanced: its geometry cannot be replaced");
            if (it->second.animated)
                ar.fail("\"" + r.path + "\" is animated: its geometry cannot be replaced");
            if (targets.count(r.path))
                ar.fail("\"" + r.path + "\" is replaced twice");
            const Eigen::Matrix4d &world = it->second.world;
            const double det = world.topLeftCorner<3, 3>().determinant();
            if (!std::isfinite(det) || std::abs(det) < 1e-12)
                ar.fail("\"" + r.path + "\" has a degenerate transform (zero scale)");
            Splicer::Target t;
            t.path = r.path;
            t.data = make_mesh_data(r.F, r.V, world.inverse());
            targets[r.path] = std::move(t);
        }

        Splicer splicer(ar, out, targets);
        root = splicer.run();
        notes = splicer.notes;
    }   /* input closed: it may now be replaced */
    out.commit(root);

    cout << "done. (took " << timeString(timer.value()) << ")" << endl;
    for (const std::string &n : notes)
        cout << "   " << n << endl;
}

} // namespace abc
