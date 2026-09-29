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

extern void
load_mesh_or_pointcloud(const std::string &filename, MatrixXu &F,
                        MatrixXf &V, MatrixXf &N,
                        const ProgressCallback &progress = ProgressCallback());

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
 * error messages.
 */
extern void build_mesh(const std::vector<Vector3f> &positions,
                       const std::vector<uint32_t> &sizes,
                       const std::vector<uint32_t> &indices,
                       MatrixXu &F, MatrixXf &V, const std::string &source);

extern void load_obj(const std::string &filename, MatrixXu &F, MatrixXf &V,
                     const ProgressCallback &progress = ProgressCallback());

extern void load_ply(const std::string &filename, MatrixXu &F, MatrixXf &V,
                     MatrixXf &N, bool pointcloud = false,
                     const ProgressCallback &progress = ProgressCallback());

extern void
load_pointcloud(const std::string &filename, MatrixXf &V, MatrixXf &N,
                const ProgressCallback &progress = ProgressCallback());

extern void write_mesh(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback());

extern void write_obj(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback());

extern void write_ply(const std::string &filename, const MatrixXu &F,
                      const MatrixXf &V,
                      const MatrixXf &N = MatrixXf(),
                      const MatrixXf &Nf = MatrixXf(),
                      const MatrixXf &UV = MatrixXf(),
                      const MatrixXf &C = MatrixXf(),
                      const ProgressCallback &progress = ProgressCallback());
