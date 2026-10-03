/*
    objscene.h: Per-object access to Wavefront OBJ files, for the per-mesh
    remeshing mode (-m / --others / --list) on .obj scenes.

    Objects are the "o" blocks of the file, or its "g" groups when it has no
    "o" line (several blocks of the same name form one object; faces before
    the first block belong to "default"). Indices may be absolute or
    negative (relative); v/vt/vn corners and "l"/"p" elements are handled.
*/

#pragma once

#include "common.h"
#include "meshio.h"
#include <functional>
#include <memory>

namespace objscene {

struct ObjectInfo {
    std::string name;
    uint64_t faces = 0;      ///< polygons ("f" lines)
    uint64_t vertices = 0;   ///< distinct positions used by those polygons
};

/// A replacement: extracted mesh (see extracted_polygons()) for an object
struct Replacement {
    std::string name;
    MatrixXu F;
    MatrixXf V;
    std::vector<CornerUVs> uvs;   ///< the first set is written ("vt")
    /// When set, F, V and uvs are left empty and fetched on demand (spool file)
    std::function<void(MatrixXu &F, MatrixXf &V, std::vector<CornerUVs> &uvs)> fetch;
};

/**
 * An OBJ file parsed once, for listing, loading and splicing its objects
 * without reading it again. Compact: the text is kept as it is, lines and
 * face corners are indexed into it (about 3x the file size in memory).
 */
class Scene {
public:
    explicit Scene(const std::string &filename);
    ~Scene();

    /// Objects that hold polygons, in file order
    std::vector<ObjectInfo> objects() const;

    /// Loads one object, triangulated like load_obj() (world space);
    /// 'uvs' receives its UV set ("vt"), per triangle corner
    void load(const std::string &name, MatrixXu &F, MatrixXf &V,
              uint64_t *polygons = nullptr, std::vector<UVSet> *uvs = nullptr) const;

    /**
     * Writes the file to 'output', replacing the polygons of the given
     * objects. Lines of the other objects are copied as they are (positions,
     * UVs, normals, materials, comments); only their face indices are
     * renumbered. A replaced object keeps its "o"/"g" line and one material
     * (its most used: OBJ faces inherit the last "usemtl", an object cannot
     * be left without one); its UVs, normals and smoothing groups are
     * dropped (they no longer match the new topology). Vertices used only
     * by replaced objects are removed, shared ones kept. 'output' may be the
     * parsed file itself (written to a temporary file, then replaced
     * atomically).
     */
    void splice(const std::string &output, const std::vector<Replacement> &replacements) const;

    struct Doc;

private:
    std::unique_ptr<Doc> d;
};

/// Shortcuts that parse the file for a single operation
std::vector<ObjectInfo> list_objects(const std::string &filename);
void load_object(const std::string &filename, const std::string &name,
                 MatrixXu &F, MatrixXf &V, uint64_t *polygons = nullptr,
                 std::vector<UVSet> *uvs = nullptr);
void splice_obj(const std::string &input, const std::string &output,
                const std::vector<Replacement> &replacements);

} // namespace objscene
