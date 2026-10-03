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
    bool keep_border = false;  ///< snap the open border back onto the input's (implies align_to_boundaries)
    enum UVMode { UVNone, UVTransfer, UVUnwrap } uv = UVNone;   ///< --uv
};

/// Parses the value of --uv ("none", "transfer", "unwrap"); throws on anything else
extern RemeshParams::UVMode parse_uv_mode(const std::string &text);

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

/// What remesh() did to its input
struct RemeshReport {
    uint64_t triangles = 0;    ///< input triangles
    uint64_t subdivided = 0;   ///< after the subdivision of a too coarse input (= triangles if none)
};

/// Remeshes a loaded mesh or point cloud (F, V, N are consumed);
/// 'polygons' is its polygon count before triangulation (for percentages)
extern void remesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
                   const RemeshParams &params, MatrixXu &F_out, MatrixXf &O_out,
                   MatrixXf &Nf_out, RemeshReport *report = nullptr);

/// Single input -> single output (whole file remeshed as one mesh)
extern void batch_process(const std::string &input, const std::string &output,
                          const RemeshParams &params);

/// Prints the polygon meshes of an Alembic file, or the objects of an OBJ file (--list);
/// 'sort' > 0 by ascending face count, < 0 descending, 0 file order; 'top' > 0 keeps
/// only the first ones
extern void batch_list(const std::string &input, int sort = 0, int top = 0);

/**
 * Alembic (or OBJ) input -> same format output, per-mesh targets (-m / --others): each
 * selected mesh is remeshed on its own and spliced back into a copy of the
 * input; unselected meshes are copied untouched. Prints the plan first;
 * with 'dryRun' stops there. With 'skipFailed', a mesh that cannot be
 * remeshed (error, no faces) is copied unchanged instead of stopping.
 * With 'progress', a progress block (weighted by input faces) is printed
 * after each mesh. With 'proxy' (USD), the selected meshes are kept and
 * the remeshed ones are added as their proxies (purpose "proxy").
 */
extern void batch_process_objects(const std::string &input, const std::string &output,
                                  const RemeshParams &params,
                                  const std::vector<MeshRule> &rules,
                                  const FaceTarget &others, bool dryRun,
                                  bool skipFailed = false, bool progress = false,
                                  bool proxy = false);
