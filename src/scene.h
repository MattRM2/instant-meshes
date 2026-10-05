/*
    scene.h: A scene file whose polygon meshes are listed, loaded one at a
    time and replaced, whatever its format (Alembic, OBJ, USD).
    Used by the per-mesh mode (-m / --others / --list).
*/

#pragma once

#include "abc.h"
#include <functional>
#include <memory>

/// A polygon mesh of a scene: its path (Alembic object path, "/name" for
/// an OBJ object), counts, flags and world transform
typedef abc::MeshSummary SceneMesh;

/// A new mesh for a scene object, in world space, with its UV sets
typedef abc::Replacement SceneReplacement;

class SceneFile {
public:
    /// Opens a scene file; throws if the format is not supported
    static std::unique_ptr<SceneFile> open(const std::string &filename);

    /// Whether 'filename' has the extension of a supported scene format
    static bool supported(const std::string &filename);

    virtual ~SceneFile() { }

    const std::string &filename() const { return mFilename; }

    /// Polygon meshes, in file order
    virtual std::vector<SceneMesh> meshes() = 0;

    /// Loads one mesh in world space, triangulated; 'uvs' receives its UV
    /// sets per triangle corner
    virtual void load(const std::string &path, MatrixXu &F, MatrixXf &V,
                      uint64_t *polygons = nullptr, std::vector<UVSet> *uvs = nullptr) = 0;

    /// The points of every mesh (world space, as load() gives them), in
    /// one pass when the format allows; the meshes that fail are left out
    virtual void mesh_points(
        const std::function<void(const std::string &path, const std::vector<Vector3f> &points)> &f);

    /// Writes the scene to 'output' (same format) with the given meshes
    /// replaced, everything else copied; 'output' may be the file itself
    virtual void write(const std::string &output, const std::vector<SceneReplacement> &replacements) = 0;

    /// USD: where the proxies of these meshes go (--proxy); throws for the
    /// formats without proxies or a mesh that cannot get one
    virtual std::vector<std::string> proxy_paths(const std::vector<std::string> &meshes) {
        (void) meshes;
        throw std::runtime_error("--proxy needs a USD scene (.usd/.usda/.usdc/.usdz)");
    }

    /// USD: writes 'output', a layer adding the given meshes as proxies
    virtual void write_proxies(const std::string &output, const std::vector<SceneReplacement> &proxies) {
        (void) output;
        (void) proxies;
        throw std::runtime_error("--proxy needs a USD scene (.usd/.usda/.usdc/.usdz)");
    }

protected:
    explicit SceneFile(const std::string &filename) : mFilename(filename) { }

    std::string mFilename;
};
