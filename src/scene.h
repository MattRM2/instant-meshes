/*
    scene.h: A scene file whose polygon meshes are listed, loaded one at a
    time and replaced, whatever its format (Alembic, OBJ; USD to come).
    Used by the per-mesh mode (-m / --others / --list).
*/

#pragma once

#include "abc.h"
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

    /// Writes the scene to 'output' (same format) with the given meshes
    /// replaced, everything else copied; 'output' may be the file itself
    virtual void write(const std::string &output, const std::vector<SceneReplacement> &replacements) = 0;

protected:
    explicit SceneFile(const std::string &filename) : mFilename(filename) { }

    std::string mFilename;
};
