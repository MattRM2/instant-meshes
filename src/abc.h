/*
    abc.h: Alembic (.abc) polygon mesh support, built directly on the Ogawa
    container (ogawa.h). Self-contained: no Alembic library.

    Alembic layout on top of Ogawa (AbcCoreOgawa):

        archive root group
          [0] data  int32 Alembic format version
          [1] data  int32 library version that wrote the file
          [2] group top object
          [3] data  archive metadata ("key=value;key=value")
          [4] data  time samplings
          [5] data  indexed metadata table
        object group
          [0]       properties (compound property group)
          [1..n]    child object groups
          [last]    data: child object headers (+ 32 bytes of hashes)
        compound property group
          [0..n-1]  sub-property groups
          [last]    data: property headers
        scalar property group: sample i data at [i]
        array property group:  sample i data at [2i], dimensions at [2i+1]
        sample data: 16-byte content hash, then the raw values

    Only the first sample of every property is read: animation is out of
    scope, an animated file is loaded at its first frame.
*/

#pragma once

#include "common.h"
#include "ogawa.h"
#include <map>

namespace abc {

typedef std::map<std::string, std::string> MetaData;

/// Plain old data types of property values (Alembic numbering)
enum Pod : uint8_t {
    PodBool = 0, PodUint8, PodInt8, PodUint16, PodInt16, PodUint32, PodInt32,
    PodUint64, PodInt64, PodFloat16, PodFloat32, PodFloat64, PodString,
    PodWstring, PodCount
};

struct Object {
    std::string name;
    std::string path;      ///< full path, "/" for the top object
    MetaData meta;
    uint64_t group = 0;    ///< Ogawa entry of the object group
    std::vector<uint8_t> rawHeader;   ///< header bytes as stored in the parent

    std::string schema() const;
};

struct Property {
    enum Type { Compound, Scalar, Array };
    std::string name;
    Type type = Compound;
    Pod pod = PodUint8;
    uint32_t extent = 1;
    uint32_t samples = 0;  ///< number of samples (scalar / array only)
    uint32_t firstChanged = 0, lastChanged = 0;  ///< stored sample range
    uint32_t timeSampling = 0;
    MetaData meta;
    uint64_t group = 0;    ///< Ogawa entry of the property group
    std::vector<uint8_t> rawHeader;   ///< header bytes as stored in the parent
};

struct TimeSampling {
    double timePerCycle = 1.0;
    std::vector<double> times;
};

class Archive {
public:
    /// Opens the file and validates the archive structure
    explicit Archive(const std::string &filename);

    ogawa::Reader &reader() { return mIn; }
    const Object &top() const { return mTop; }

    /// Child objects, in file order
    std::vector<Object> children(const Object &object);

    /// Top-level compound property of an object
    Property properties(const Object &object);

    /// Sub-properties of a compound property, in file order
    std::vector<Property> properties(const Property &compound);

    /// Sub-property by name (false if absent)
    bool find(const Property &compound, const std::string &name, Property &out);

    /// Raw bytes of the first sample of a scalar or array property (without
    /// the content hash). The size is checked to be a whole number of
    /// values; returns an empty buffer for a property without samples.
    std::vector<uint8_t> sample(const Property &property);

    /// First sample converted to doubles / 32-bit indices / a string
    std::vector<double> sample_doubles(const Property &property);
    std::vector<uint32_t> sample_indices(const Property &property);
    std::string sample_string(const Property &property);

    /// Object at a full path such as "/Props/MeshA" (false if absent)
    bool resolve(const std::string &path, Object &out);

    const std::vector<TimeSampling> &time_samplings() const { return mTimeSamplings; }

    /// Indexed metadata table as stored (entry i has index i + 1)
    const std::vector<std::string> &indexed_metadata() const { return mIndexedRaw; }

    /// Index of the stored data of sample 'index' (samples that repeat the
    /// previous value are not stored again)
    uint32_t stored_index(const Property &property, uint32_t index) const;

    [[noreturn]] void fail(const std::string &msg) const { mIn.fail(msg); }

private:
    MetaData parse_metadata(const std::string &text) const;

    ogawa::Reader mIn;
    Object mTop;
    std::vector<MetaData> mIndexedMeta;
    std::vector<std::string> mIndexedRaw;
    std::vector<TimeSampling> mTimeSamplings;
};

/// Byte size of one value of a POD type (0 for strings)
uint32_t pod_size(Pod pod);

/// "key=value;key=value" with sorted keys, as written by Alembic
std::string serialize(const MetaData &meta);

struct MeshSummary {
    std::string path;      ///< full path of the PolyMesh object
    uint64_t vertices = 0;
    uint64_t faces = 0;    ///< polygons, as shown in a DCC (not triangles)
    bool animated = false; ///< positions or topology have several samples
    bool instanced = false;///< reached through an instance, or instanced elsewhere
    Eigen::Matrix4d world = Eigen::Matrix4d::Identity();  ///< first sample
};

/// Polygon meshes of an archive, in traversal order
std::vector<MeshSummary> list_meshes(const std::string &filename);

/// Loads exactly the PolyMesh at 'path' (no sub-objects), in world space
void load_abc_mesh(const std::string &filename, const std::string &path,
                   MatrixXu &F, MatrixXf &V, uint64_t *polygons = nullptr);

/// Wildcard match: '*' = any sequence of characters, '?' = one character
bool glob_match(const std::string &pattern, const std::string &text);

/// A replacement mesh for splice_abc(): extracted mesh (see
/// extracted_polygons()) in world space, for the PolyMesh at 'path'
struct Replacement {
    std::string path;
    MatrixXu F;
    MatrixXf V;
};

/**
 * Copies the archive 'input' to 'output', replacing the geometry of the
 * given PolyMesh objects. Everything else (other objects, transforms,
 * metadata, animation) is copied block for block. For a replaced mesh:
 * name, parent transform, object properties and user properties are kept;
 * positions are brought back from world to the mesh's local space; normals,
 * UVs and vertex / face attributes are dropped (they no longer match the
 * topology); a single face set is rebuilt over all new faces, several face
 * sets are dropped. Hashes are recomputed along the modified paths.
 * 'output' may be 'input': the input is closed before the atomic
 * replacement.
 */
void splice_abc(const std::string &input, const std::string &output,
                const std::vector<Replacement> &replacements);

/**
 * Loads the polygon meshes of an archive in world space, triangulated and
 * indexed exactly like OBJ files (see build_mesh()). 'object' restricts
 * loading to the meshes at or below that full path ("" loads everything).
 * Non-polygon objects (cameras, curves, points, SubD, ...) are skipped.
 * 'polygons' receives the number of loaded polygons before triangulation.
 */
void load_abc(const std::string &filename, MatrixXu &F, MatrixXf &V,
              const std::string &object = "",
              const ProgressCallback &progress = ProgressCallback(),
              uint64_t *polygons = nullptr);

/**
 * Writes an extracted mesh (see extracted_polygons()) as a new Alembic
 * archive: one Xform with a PolyMesh of the same name below it, named after
 * the file. Polygons are written clockwise, as Alembic expects. The file is
 * replaced atomically (see ogawa::Writer).
 */
void write_abc(const std::string &filename, const MatrixXu &F, const MatrixXf &V,
               const ProgressCallback &progress = ProgressCallback());

/**
 * Recomputes every sample key (MurmurHash3) and object hash (SpookyHash)
 * of an archive and compares them with the stored ones. Returns the number
 * of objects checked; throws on the first mismatch.
 */
uint64_t verify_hashes(const std::string &filename);

} // namespace abc
