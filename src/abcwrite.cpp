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
#include <cfloat>

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

class Verifier {
public:
    explicit Verifier(Archive &ar) : mAr(ar), mIn(ar.reader()) { }

    uint64_t run() {
        object(mAr.top());
        return mObjects;
    }

private:
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

        /* Stored hashes: last 32 bytes of the child headers */
        std::vector<uint64_t> g = mIn.group(o.group);
        if (g.empty() || !is_data(g.back()) || mIn.data_size(g.back()) < 32)
            mAr.fail("object \"" + o.path + "\" has no stored hashes");
        Digest stored[2];
        mIn.read_data(g.back(), mIn.data_size(g.back()) - 32, 32, stored);
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
    explicit ArchiveWriter(ogawa::Writer &out) : mOut(out) { }

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

        std::vector<uint8_t> table;
        for (const std::string &s : mMetaTable) {
            table.push_back((uint8_t) s.size());
            table.insert(table.end(), s.begin(), s.end());
        }
        const uint64_t indexed = mOut.add_data(table.data(), table.size());

        mOut.commit(mOut.add_group({ v0, v1, topGroup, metaData, timeSamplings, indexed }));
    }

private:
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

} // namespace

uint64_t verify_hashes(const std::string &filename) {
    Archive ar(filename);
    return Verifier(ar).run();
}

void write_abc(const std::string &filename, const MatrixXu &F, const MatrixXf &V,
               const ProgressCallback &progress) {
    Timer<> timer;
    cout << "Writing \"" << filename << "\" (V=" << V.cols() << ", F=" << F.cols() << ") .. ";
    cout.flush();

    std::vector<uint32_t> sizes, ccw, faceIds;
    const size_t irregular = extracted_polygons(F, sizes, ccw, faceIds);
    if (sizes.empty() || V.cols() == 0)
        throw std::runtime_error("Alembic writer: the mesh is empty!");
    if (V.cols() > 0x7fffffff || ccw.size() > 0x7fffffff)
        throw std::runtime_error("Alembic writer: the mesh is too large!");

    /* Keep only the vertices used by polygons, in their original order:
       quad-dominant extraction leaves the centers of irregular faces unused
       (Blender's OBJ importer drops them too) */
    std::vector<int32_t> remap((size_t) V.cols(), -1);
    for (uint32_t index : ccw) {
        if (index >= V.cols())
            throw std::runtime_error("Alembic writer: vertex index out of range!");
        remap[index] = 0;
    }
    std::vector<float> P;
    double bounds[6] = { DBL_MAX, DBL_MAX, DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX };
    int32_t used = 0;
    for (uint32_t i = 0; i < V.cols(); ++i) {
        if (remap[i] < 0)
            continue;
        remap[i] = used++;
        for (int k = 0; k < 3; ++k) {
            const float x = (float) V(k, i);
            if (!std::isfinite(x))
                throw std::runtime_error("Alembic writer: invalid vertex position!");
            P.push_back(x);
            bounds[k] = std::min(bounds[k], (double) x);
            bounds[k + 3] = std::max(bounds[k + 3], (double) x);
        }
    }

    /* Clockwise faces */
    std::vector<int32_t> faceIndices(ccw.size()), faceCounts(sizes.size());
    size_t offset = 0;
    for (size_t f = 0; f < sizes.size(); ++f) {
        faceCounts[f] = (int32_t) sizes[f];
        for (uint32_t k = 0; k < sizes[f]; ++k)
            faceIndices[offset + k] = remap[ccw[offset + sizes[f] - 1 - k]];
        offset += sizes[f];
    }

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
    mesh.properties.push_back(make_compound(".geom", polyMeta, {
        make_value(".selfBnds", Property::Scalar, PodFloat64, 6, bounds, 6, {{ "interpretation", "box" }}),
        make_value("P", Property::Array, PodFloat32, 3, P.data(), P.size(),
                   {{ "geoScope", "vtx" }, { "interpretation", "point" }}),
        make_value(".faceIndices", Property::Array, PodInt32, 1, faceIndices.data(), faceIndices.size()),
        make_value(".faceCounts", Property::Array, PodInt32, 1, faceCounts.data(), faceCounts.size())
    }));

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
    if (irregular > 0)
        cout << irregular << " irregular faces, ";
    cout << "took " << timeString(timer.value()) << ")" << endl;
}

} // namespace abc
