/*
    batch.h -- command line interface to Instant Meshes

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#pragma once

#include "common.h"

/// Remeshing settings shared by all batch modes
struct RemeshParams {
    int rosy = 4, posy = 4;
    Float scale = -1;
    int face_count = -1;
    Float face_percent = -1;   ///< % of the input polygon count (overrides face_count)
    int vertex_count = -1;
    Float crease_angle = -1;
    bool extrinsic = true, align_to_boundaries = false;
    int smooth_iter = 2, knn_points = 10;
    bool pure_quad = true, deterministic = false;
};

/// Face target of one mesh: a percentage of its polygons or a face count
struct FaceTarget {
    Float percent = -1;
    int count = -1;
    std::string text;          ///< as given on the command line ("75%", "5000")

    bool valid() const { return percent > 0 || count > 0; }
};

/// Parses "75%" or "5000"; throws on anything else
extern FaceTarget parse_face_target(const std::string &text);

/// -m rule: meshes whose path or ancestors match 'pattern' get 'target'
struct MeshRule {
    std::string pattern;       ///< wildcards * and ?, optional leading '/'
    FaceTarget target;
    std::string text;          ///< the whole argument, for messages
};

/// Parses "pattern=target" (the last '=' separates them); throws if invalid
extern MeshRule parse_mesh_rule(const std::string &text);

/// Whether a rule pattern selects the mesh at 'path' (the mesh itself or
/// one of its ancestors matches; a pattern with '/' is a path from the root)
extern bool rule_matches(const std::string &pattern, const std::string &path);

/// Remeshes a loaded mesh or point cloud (F, V, N are consumed);
/// 'polygons' is its polygon count before triangulation (for percentages)
extern void remesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
                   const RemeshParams &params, MatrixXu &F_out, MatrixXf &O_out,
                   MatrixXf &Nf_out);

/// Single input -> single output (whole file remeshed as one mesh)
extern void batch_process(const std::string &input, const std::string &output,
                          const RemeshParams &params);

/// Prints the polygon meshes of an Alembic file, or the objects of an OBJ file (--list)
extern void batch_list(const std::string &input);

/**
 * Alembic (or OBJ) input -> same format output, per-mesh targets (-m / --others): each
 * selected mesh is remeshed on its own and spliced back into a copy of the
 * input; unselected meshes are copied untouched. Prints the plan first;
 * with 'dryRun' stops there.
 */
extern void batch_process_objects(const std::string &input, const std::string &output,
                                  const RemeshParams &params,
                                  const std::vector<MeshRule> &rules,
                                  const FaceTarget &others, bool dryRun);
