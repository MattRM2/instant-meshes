/*
    usdscene.h: the meshes of a USD layer (usd.h), for the per-mesh mode and
    the whole-file mode, and the .usda layers Instant Meshes writes.

    The layer is read on its own (composition comes later): defined prims
    ("def") are walked, classes, overs and inactive prims are not; the
    transforms follow xformOpOrder (every op type, !invert!, the first time
    sample of an animated op, !resetXformStack!). A mesh below an
    instanceable prim or a PointInstancer is flagged as instanced; one with
    time-sampled topology or points as animated.
*/

#pragma once

#include "usd.h"
#include "abc.h"
#include "meshio.h"

namespace usd {

/// The polygon meshes (prims of type Mesh), in traversal order
std::vector<abc::MeshSummary> list_meshes(const Layer &layer);

/// One mesh in world space, triangulated (counter-clockwise); 'uvs': its
/// texture coordinate primvars (texCoord2*, or float2 st / uv)
void load_mesh(const Layer &layer, const std::string &path, MatrixXu &F, MatrixXf &V,
               uint64_t *polygons = nullptr, std::vector<UVSet> *uvs = nullptr);

/// Every mesh that is not instanced, merged (whole-file mode); a UV set is
/// kept if every mesh has it
void load_all(const Layer &layer, MatrixXu &F, MatrixXf &V, uint64_t *polygons = nullptr,
              std::vector<UVSet> *uvs = nullptr);

/**
 * Writes 'output' (.usda), a layer over 'layer': it sublayers the input and
 * overrides the given meshes. For a replaced mesh: points (in the mesh's own
 * space), faces, extent and orientation are written, the new UV sets keep
 * their primvar names; normals and every primvar that is not constant are
 * blocked (they no longer match the topology), as well as creases, corners
 * and holes; GeomSubsets are deactivated and the mesh is bound to the most
 * used of their materials. The stage metadata of the input is copied.
 */
void write_overlay(const Layer &layer, const std::string &output,
                   const std::vector<abc::Replacement> &replacements);

/// A new .usda stage holding one extracted mesh (whole-file output)
void write_usda(const std::string &filename, const MatrixXu &F, const MatrixXf &V,
                const std::vector<CornerUVs> &uvs = std::vector<CornerUVs>());

/// Whether a file name has a USD extension (.usd, .usda, .usdc, .usdz)
bool is_usd_file(const std::string &filename);

} // namespace usd
