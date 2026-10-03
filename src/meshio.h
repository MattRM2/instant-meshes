/*
    meshio.h: Mesh file input/output routines

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include "common.h"

/**
 * A UV set of a mesh: 'values' (2 x N) and, for every corner, the index of
 * its value. As read from a file, the corners are the polygon corners in
 * the order given to build_mesh(); build_mesh() turns them into triangle
 * corners (3 per column of F: corner j of triangle t at 3t + j).
 */
struct UVSet {
    std::string name;      ///< "UVMap", "Planar"... ("uv" when the file has no name)
    MatrixXf values;
    std::vector<uint32_t> corners;
};

/**
 * A UV set of an extracted mesh (F with 3 or 4 rows, see
 * extracted_polygons()): the UV of corner j of column f is column
 * f * F.rows() + j of 'corners'. An irregular column (a directed edge of a
 * larger polygon) only uses its corner 0, the vertex F(0, f).
 */
struct CornerUVs {
    std::string name;
    MatrixXf corners;
};

/// A UV set of a loaded triangle mesh as CornerUVs (writing it back as is)
extern CornerUVs corner_uvs(const UVSet &set, const MatrixXu &F);

/// Units and up axis of a scene: the stage metadata of a USD file; the
/// other formats carry none, 1 unit = 1 meter and Y up (as Blender reads
/// and writes OBJ and Alembic files)
struct SceneUnits {
    double metersPerUnit = 1.0;
    std::string upAxis = "Y";
};

/// Loads a mesh (.obj/.ply/.abc/.usd) or point cloud (.aln). If 'polygons' is
/// given, it receives the number of polygons of the file before
/// triangulation (as shown in a DCC), 0 for a point cloud. If 'uvs' is
/// given, it receives the UV sets of the mesh, per triangle corner (OBJ and
/// Alembic; a set is kept only if every loaded mesh has it). 'units'
/// receives the units of the file (see SceneUnits).
extern void
load_mesh_or_pointcloud(const std::string &filename, MatrixXu &F,
                        MatrixXf &V, MatrixXf &N,
                        const ProgressCallback &progress = ProgressCallback(),
                        uint64_t *polygons = nullptr,
                        std::vector<UVSet> *uvs = nullptr,
                        SceneUnits *units = nullptr);

/**
 * Split a polygon with n >= 3 corners (positions given in corner order) into
 * n-2 triangles, appended to 'tris' as corner indices in [0, n). Triangles
 * and quads keep the historical split (0,1,2) (3,0,2); larger polygons are
 * ear-clipped in their best-fit plane, falling back to a fan when the
 * polygon is degenerate or self-intersecting. Shared by all mesh readers.
 */
extern void triangulate_polygon(const std::vector<Vector3f> &p,
                                std::vector<uint32_t> &tris);

/**
 * Triangulates polygons ('sizes' corners each, 'indices' into 'positions')
 * with triangulate_polygon() and builds F/V, numbering vertices in order of
 * first use (unreferenced positions are dropped). Shared by all polygon mesh
 * readers so that they behave identically. 'source' names the file in
 * error messages. The UV sets in 'uvs', given per polygon corner, come out
 * per triangle corner.
 */
extern void build_mesh(const std::vector<Vector3f> &positions,
                       const std::vector<uint32_t> &sizes,
                       const std::vector<uint32_t> &indices,
                       MatrixXu &F, MatrixXf &V, const std::string &source,
                       std::vector<UVSet> *uvs = nullptr);

extern void load_obj(const std::string &filename, MatrixXu &F, MatrixXf &V,
                     const ProgressCallback &progress = ProgressCallback(),
                     uint64_t *polygons = nullptr,
                     std::vector<UVSet> *uvs = nullptr);

extern void load_ply(const std::string &filename, MatrixXu &F, MatrixXf &V,
                     MatrixXf &N, bool pointcloud = false,
                     const ProgressCallback &progress = ProgressCallback());

extern void
load_pointcloud(const std::string &filename, MatrixXf &V, MatrixXf &N,
                const ProgressCallback &progress = ProgressCallback());

/// 'uvs': UV sets of the extracted mesh (OBJ: the first one; Alembic: the
/// first one as .geom/uv, the others in .arbGeomParams; PLY: none);
/// 'units': written as the stage metadata of a .usda
extern void write_mesh(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback(),
                      const std::vector<CornerUVs> &uvs = std::vector<CornerUVs>(),
                      const SceneUnits &units = SceneUnits());

/**
 * Polygons of an extracted mesh (F with 3 or 4 rows; a quad with F(2) ==
 * F(3) is one directed edge F(0) -> F(1) of a larger polygon, see
 * extract_faces()), in output order: regular faces first, then the
 * reassembled polygons. 'faceIds' gives the column of F each polygon comes
 * from (for face normals). Returns the number of reassembled polygons.
 * 'cornerIds', if given, receives for every listed corner its corner id in
 * F (f * F.rows() + j, see CornerUVs). Shared by the OBJ and Alembic writers.
 */
extern size_t extracted_polygons(const MatrixXu &F, std::vector<uint32_t> &sizes,
                                 std::vector<uint32_t> &indices,
                                 std::vector<uint32_t> &faceIds,
                                 std::vector<uint32_t> *cornerIds = nullptr);

/// A UV set for the corners listed by extracted_polygons(): its distinct
/// values (u, v pairs) and, for every corner, the index of its value
extern void indexed_uvs(const CornerUVs &set, const std::vector<uint32_t> &cornerIds,
                        std::vector<float> &values, std::vector<uint32_t> &index);

extern void write_obj(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback(),
                      const std::vector<CornerUVs> &uvs = std::vector<CornerUVs>());

extern void write_ply(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback());
