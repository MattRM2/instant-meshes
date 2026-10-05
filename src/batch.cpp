/*
    batch.cpp -- command line interface to Instant Meshes

    This file is part of the implementation of

        Instant Field-Aligned Meshes
        Wenzel Jakob, Daniele Panozzo, Marco Tarini, and Olga Sorkine-Hornung
        In ACM Transactions on Graphics (Proc. SIGGRAPH Asia 2015)

    All rights reserved. Use of this source code is governed by a
    BSD-style license that can be found in the LICENSE.txt file.
*/

#include "batch.h"
#include "meshio.h"
#include "abc.h"
#include "objscene.h"
#include "dedge.h"
#include "subdivide.h"
#include "meshstats.h"
#include "hierarchy.h"
#include "field.h"
#include "normal.h"
#include "extract.h"
#include "bvh.h"
#include "border.h"
#include "scene.h"
#include "spool.h"
#include "project.h"
#include "usdscene.h"
#include "uvtransfer.h"
#include "uvunwrap.h"
#include <iomanip>
#include <cmath>
#include <limits>
#include <fstream>
#include <functional>
#include <memory>

/* ------------------------------------------------------------------------- */
/*  Face targets and mesh rules                                              */
/* ------------------------------------------------------------------------- */

FaceTarget parse_face_target(const std::string &text) {
    FaceTarget t;
    t.text = text;
    try {
        if (!text.empty() && text.back() == '%')
            t.percent = str_to_float(text.substr(0, text.size() - 1));
        else
            t.count = str_to_int32_t(text);
    } catch (const std::exception &) {
        throw std::runtime_error("Invalid target \"" + text + "\": a percentage of the polygons (50%) or a "
                                 "face count (5000)");
    }
    if (!text.empty() && text.back() == '%') {
        if (!std::isfinite(t.percent) || !(t.percent > 0))
            throw std::runtime_error("Invalid face percentage \"" + text + "\"");
    } else if (t.count <= 0) {
        throw std::runtime_error("Invalid face count \"" + text + "\"");
    }
    return t;
}

MeshRule parse_mesh_rule(const std::string &text) {
    const size_t eq = text.rfind('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 == text.size())
        throw std::runtime_error("Invalid mesh rule \"" + text + "\" (expected name=target, e.g. MeshA=75%)");
    MeshRule rule;
    rule.text = text;
    rule.pattern = text.substr(0, eq);
    while (!rule.pattern.empty() && rule.pattern[0] == '/')
        rule.pattern.erase(0, 1);
    if (rule.pattern.empty() || rule.pattern.find("//") != std::string::npos ||
        rule.pattern.back() == '/')
        throw std::runtime_error("Invalid object name or path in \"" + text + "\"");
    rule.target = parse_face_target(text.substr(eq + 1));
    return rule;
}

bool rule_matches(const std::string &pattern, const std::string &path) {
    const std::vector<std::string> names = str_tokenize(path, '/', false);
    if (pattern.find('/') == std::string::npos) {
        /* A name: the mesh or any of its ancestors */
        for (const std::string &name : names)
            if (abc::glob_match(pattern, name))
                return true;
        return false;
    }
    /* A path from the root: the object at that depth, the mesh at or below */
    const std::vector<std::string> parts = str_tokenize(pattern, '/', false);
    if (parts.size() > names.size())
        return false;
    for (size_t i = 0; i < parts.size(); ++i)
        if (!abc::glob_match(parts[i], names[i]))
            return false;
    return true;
}

RemeshParams::UVMode parse_uv_mode(const std::string &text) {
    const std::string mode = str_tolower(text);
    if (mode == "none")
        return RemeshParams::UVNone;
    if (mode == "transfer")
        return RemeshParams::UVTransfer;
    if (mode == "unwrap")
        return RemeshParams::UVUnwrap;
    throw std::runtime_error("Invalid --uv mode \"" + text + "\" (none, transfer or unwrap)");
}

/* --uv unwrap: new UVs for the new mesh (xatlas) */
static std::vector<CornerUVs> unwrap(const MatrixXu &F, const MatrixXf &O) {
    Timer<> timer;
    UnwrapStats stats;
    std::vector<CornerUVs> result { unwrap_uvs(F, O, "UVMap", &stats) };
    cout << "UV unwrap: " << stats.charts << " charts, " << (int) std::round(stats.utilization * 100)
         << "% of the UV square used";
    if (stats.split > 0)
        cout << ", " << stats.split << " polygons kept whole across chart borders (re-packed)";
    cout << ". (took " << timeString(timer.value()) << ")" << endl;
    return result;
}

/* --uv transfer: the UV sets of the original, carried over to the new mesh */
static std::vector<CornerUVs> transfer_uvs(const MatrixXu &F0, const MatrixXf &V0, const std::vector<UVSet> &uvs,
                                           const MatrixXu &F, const MatrixXf &O, const std::string &what) {
    if (uvs.empty()) {
        cout << "UV transfer: " << what << " has no UVs, nothing to transfer." << endl;
        return std::vector<CornerUVs>();
    }
    Timer<> timer;
    UVTransfer transfer(F0, V0, uvs);
    UVTransfer::Stats stats;
    std::vector<CornerUVs> result = transfer.transfer(F, O, &stats);
    std::string names;
    for (const UVSet &s : uvs)
        names += (names.empty() ? "" : ", ") + s.name;
    cout << "UV transfer: " << uvs.size() << " set" << (uvs.size() > 1 ? "s" : "") << " (" << names << ") onto "
         << stats.corners << " corners, " << stats.extended << " extended past a UV seam";
    if (stats.flipped > 0)
        cout << ", " << stats.flipped << " faces without a source facing the same way";
    cout << ". (took " << timeString(timer.value()) << ")" << endl;
    return result;
}

static void print_settings(const RemeshParams &p) {
    cout << "   Rotation symmetry type = " << p.rosy << endl;
    cout << "   Position symmetry type = " << (p.posy==3?6:p.posy) << endl;
    cout << "   Crease angle threshold = ";
    if (p.crease_angle > 0)
        cout << p.crease_angle << endl;
    else
        cout << "disabled" << endl;
    cout << "   Extrinsic mode         = " << (p.extrinsic ? "enabled" : "disabled") << endl;
    cout << "   Align to boundaries    = " << (p.align_to_boundaries || p.keep_border ? "yes" : "no") << endl;
    if (p.keep_border)
        cout << "   Keep border            = yes (snapped onto the input border)" << endl;
    if (p.uv == RemeshParams::UVTransfer)
        cout << "   UVs                    = transferred from the input" << endl;
    if (p.uv == RemeshParams::UVUnwrap)
        cout << "   UVs                    = unwrapped (xatlas)" << endl;
    cout << "   kNN points             = " << p.knn_points << " (only applies to point clouds)"<< endl;
    cout << "   Fully deterministic    = " << (p.deterministic ? "yes" : "no") << endl;
    if (p.posy == 4)
        cout << "   Output mode            = " << (p.pure_quad ? "pure quad mesh" : "quad-dominant mesh") << endl;
    if (p.face_percent > 0)
        cout << "   Face target            = " << p.face_percent << "% of the input polygons" << endl;
}

/* ------------------------------------------------------------------------- */
/*  Remeshing                                                                */
/* ------------------------------------------------------------------------- */

/* One remeshing at the given target (see remesh()) */
static void remesh_once(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
                        const RemeshParams &params, MatrixXu &F_extr, MatrixXf &O_extr,
                        MatrixXf &Nf_extr, RemeshReport *report) {
    const int rosy = params.rosy, posy = params.posy;
    Float scale = params.scale, face_percent = params.face_percent;
    int face_count = params.face_count, vertex_count = params.vertex_count;
    const Float creaseAngle = params.crease_angle;
    const bool extrinsic = params.extrinsic;
    const bool align_to_boundaries = params.align_to_boundaries || params.keep_border;
    const int smooth_iter = params.smooth_iter, knn_points = params.knn_points;
    const bool pure_quad = params.pure_quad, deterministic = params.deterministic;

    VectorXf A;
    std::set<uint32_t> crease_in, crease_out;
    BVH *bvh = nullptr;
    AdjacencyMatrix adj = nullptr;
    struct BVHGuard {
        BVH *&bvh;
        ~BVHGuard() { delete bvh; }
    } bvhGuard { bvh };

    bool pointcloud = F.size() == 0;

    /* --keep-border: the input border, before any subdivision */
    BorderCurves border;
    if (params.keep_border && !pointcloud)
        border = extract_border(F, V);

    if (face_percent > 0) {
        if (pointcloud || polygons == 0)
            throw std::runtime_error("A percentage face target needs a polygon mesh as input, not a point cloud!");
        /* In pure quad mode the extracted mesh is subdivided afterwards
           (every n-gon becomes n quads, ~4x the faces): aim for a quarter
           so that the final mesh matches the requested percentage */
        const bool subdivided = posy == 4 && pure_quad;
        const double target = polygons * (double) face_percent / 100.0;
        const double extracted = std::round(subdivided ? target / 4 : target);
        if (extracted > 1e9)
            throw std::runtime_error("The percentage face target is too large!");
        face_count = std::max(1, (int) extracted);
        cout << "Face target: " << face_percent << "% of " << polygons << " input polygons = ~"
             << (uint64_t) std::round(target) << " faces";
        if (subdivided)
            cout << " (~" << face_count << " extracted, then subdivided into quads)";
        cout << endl;
    }

    Timer<> timer;
    MeshStats stats = compute_mesh_stats(F, V, deterministic);

    if (pointcloud) {
        bvh = new BVH(&F, &V, &N, stats.mAABB);
        bvh->build();
        adj = generate_adjacency_matrix_pointcloud(V, N, bvh, stats, knn_points, deterministic);
        A.resize(V.cols());
        A.setConstant(1.0f);
    }

    if (scale < 0 && vertex_count < 0 && face_count < 0) {
        cout << "No target vertex count/face count/scale argument provided. "
                "Setting to the default of 1/16 * input vertex count." << endl;
        vertex_count = V.cols() / 16;
    }

    if (scale > 0) {
        Float face_area = posy == 4 ? (scale*scale) : (std::sqrt(3.f)/4.f*scale*scale);
        face_count = stats.mSurfaceArea / face_area;
        vertex_count = posy == 4 ? face_count : (face_count / 2);
    } else if (face_count > 0) {
        Float face_area = stats.mSurfaceArea / face_count;
        vertex_count = posy == 4 ? face_count : (face_count / 2);
        scale = posy == 4 ? std::sqrt(face_area) : (2*std::sqrt(face_area * std::sqrt(1.f/3.f)));
    } else if (vertex_count > 0) {
        face_count = posy == 4 ? vertex_count : (vertex_count * 2);
        Float face_area = stats.mSurfaceArea / face_count;
        scale = posy == 4 ? std::sqrt(face_area) : (2*std::sqrt(face_area * std::sqrt(1.f/3.f)));
    }

    cout << "Output mesh goals (approximate)" << endl;
    cout << "   Vertex count           = " << vertex_count << endl;
    cout << "   Face count             = " << face_count << endl;
    cout << "   Edge length            = " << scale << endl;

    MultiResolutionHierarchy mRes;
    /* The hierarchy allocates its adjacency by hand and has no destructor:
       release it whatever happens, or every remeshed object of a scene
       would stay in memory */
    struct HierarchyGuard {
        MultiResolutionHierarchy &h;
        ~HierarchyGuard() { h.free(); }
    } hierarchyGuard { mRes };

    if (report)
        report->triangles = report->subdivided = (uint64_t) F.cols();

    if (!pointcloud) {
        /* Subdivide the mesh if necessary */
        VectorXu V2E, E2E;
        VectorXb boundary, nonManifold;
        if (stats.mMaximumEdgeLength*2 > scale || stats.mMaximumEdgeLength > stats.mAverageEdgeLength * 2) {
            cout << "Input mesh is too coarse for the desired output edge length "
                    "(max input mesh edge length=" << stats.mMaximumEdgeLength
                 << "), subdividing .." << endl;
            const uint64_t before = (uint64_t) F.cols();
            build_dedge(F, V, V2E, E2E, boundary, nonManifold);
            subdivide(F, V, V2E, E2E, boundary, nonManifold, std::min(scale/2, (Float) stats.mAverageEdgeLength*2), deterministic);
            const uint64_t after = (uint64_t) F.cols();
            if (report)
                report->subdivided = after;
            if (after > before)
                cout << "Warning: the input was subdivided from " << before << " to " << after
                     << " triangles (x" << std::fixed << std::setprecision(1) << (double) after / before
                     << std::defaultfloat << std::setprecision(6)
                     << "): its longest edges are much longer than the target edge length, "
                        "memory and time grow accordingly." << endl;
        }

        /* Compute a directed edge data structure */
        build_dedge(F, V, V2E, E2E, boundary, nonManifold);

        /* Compute adjacency matrix */
        adj = generate_adjacency_matrix_uniform(F, V2E, E2E, nonManifold);

        /* Compute vertex/crease normals */
        if (creaseAngle >= 0)
            generate_crease_normals(F, V, V2E, E2E, boundary, nonManifold, creaseAngle, N, crease_in);
        else
            generate_smooth_normals(F, V, V2E, E2E, nonManifold, N);

        /* Compute dual vertex areas */
        compute_dual_vertex_areas(F, V, V2E, E2E, nonManifold, A);

        mRes.setE2E(std::move(E2E));
    }

    /* Build multi-resolution hierarrchy */
    mRes.setAdj(std::move(adj));
    mRes.setF(std::move(F));
    mRes.setV(std::move(V));
    mRes.setA(std::move(A));
    mRes.setN(std::move(N));
    mRes.setScale(scale);
    mRes.build(deterministic);
    mRes.resetSolution();

    if (align_to_boundaries && !pointcloud) {
        mRes.clearConstraints();
        for (uint32_t i=0; i<3*mRes.F().cols(); ++i) {
            if (mRes.E2E()[i] == INVALID) {
                uint32_t i0 = mRes.F()(i%3, i/3);
                uint32_t i1 = mRes.F()((i+1)%3, i/3);
                Vector3f p0 = mRes.V().col(i0), p1 = mRes.V().col(i1);
                Vector3f edge = p1-p0;
                if (edge.squaredNorm() > 0) {
                    edge.normalize();
                    mRes.CO().col(i0) = p0;
                    mRes.CO().col(i1) = p1;
                    mRes.CQ().col(i0) = mRes.CQ().col(i1) = edge;
                    mRes.CQw()[i0] = mRes.CQw()[i1] = mRes.COw()[i0] =
                        mRes.COw()[i1] = 1.0f;
                }
            }
        }
        mRes.propagateConstraints(rosy, posy);
    }

    if (bvh) {
        bvh->setData(&mRes.F(), &mRes.V(), &mRes.N());
    } else if (smooth_iter > 0) {
        bvh = new BVH(&mRes.F(), &mRes.V(), &mRes.N(), stats.mAABB);
        bvh->build();
    }

    cout << "Preprocessing is done. (total time excluding file I/O: "
         << timeString(timer.reset()) << ")" << endl;

    Optimizer optimizer(mRes, false);
    optimizer.setRoSy(rosy);
    optimizer.setPoSy(posy);
    optimizer.setExtrinsic(extrinsic);

    cout << "Optimizing orientation field .. ";
    cout.flush();
    optimizer.optimizeOrientations(-1);
    optimizer.notify();
    optimizer.wait();
    cout << "done. (took " << timeString(timer.reset()) << ")" << endl;

    std::map<uint32_t, uint32_t> sing;
    compute_orientation_singularities(mRes, sing, extrinsic, rosy);
    cout << "Orientation field has " << sing.size() << " singularities." << endl;
    timer.reset();

    cout << "Optimizing position field .. ";
    cout.flush();
    optimizer.optimizePositions(-1);
    optimizer.notify();
    optimizer.wait();
    cout << "done. (took " << timeString(timer.reset()) << ")" << endl;
    
    //std::map<uint32_t, Vector2i> pos_sing;
    //compute_position_singularities(mRes, sing, pos_sing, extrinsic, rosy, posy);
    //cout << "Position field has " << pos_sing.size() << " singularities." << endl;
    //timer.reset();

    optimizer.shutdown();

    MatrixXf N_extr;
    std::vector<std::vector<TaggedLink>> adj_extr;
    extract_graph(mRes, extrinsic, rosy, posy, adj_extr, O_extr, N_extr,
                  crease_in, crease_out, deterministic);

    extract_faces(adj_extr, O_extr, N_extr, Nf_extr, F_extr, posy,
            mRes.scale(), crease_out, true, pure_quad, bvh, smooth_iter);
    cout << "Extraction is done. (total time: " << timeString(timer.reset()) << ")" << endl;

    if (params.keep_border && !pointcloud) {
        if (border.empty()) {
            cout << "Keep border: the input has no open border." << endl;
        } else {
            const BorderSnapStats s = snap_to_border(border, mRes.scale(), F_extr, O_extr, Nf_extr);
            cout << "Keep border: " << s.snapped << " of " << s.borderVertices
                 << " border vertices snapped onto the input border, " << s.inserted
                 << " input corners added (" << s.faces << " faces)";
            if (s.tooFar > 0)
                cout << ", " << s.tooFar << " too far from it, left in place";
            cout << ". (took " << timeString(timer.reset()) << ")" << endl;
        }
    }
}

/* A percentage target: when the result misses it widely (an object thinner
   than the edge length, whose sides the extraction merges: boxes, bags,
   panels), the remeshing runs again: the target scaled by the gap, then,
   once an attempt fell short and another went over, between the two (the
   face count jumps when the sides separate); at most 5 attempts, the one
   nearest to the target is kept */
void remesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
            const RemeshParams &params, MatrixXu &F_extr, MatrixXf &O_extr,
            MatrixXf &Nf_extr, RemeshReport *report) {
    if (!(params.face_percent > 0) || F.size() == 0 || polygons == 0) {
        remesh_once(F, V, N, polygons, params, F_extr, O_extr, Nf_extr, report);
        return;
    }
    const double target = polygons * (double) params.face_percent / 100.0;
    const MatrixXu F0 = F;
    const MatrixXf V0 = V, N0 = N;
    RemeshParams p = params;
    double bestError = std::numeric_limits<double>::infinity();
    double under = -1, over = -1;   /* percentages that gave too few / too many faces */
    const int attempts = 5;
    for (int attempt = 0; attempt < attempts; ++attempt) {
        MatrixXu Fi = attempt == 0 ? std::move(F) : F0, Fo;
        MatrixXf Vi = attempt == 0 ? std::move(V) : V0, Ni = attempt == 0 ? std::move(N) : N0, Oo, Nfo;
        RemeshReport r;
        remesh_once(Fi, Vi, Ni, polygons, p, Fo, Oo, Nfo, &r);
        const double got = (double) Fo.cols(), ratio = got / target;
        const double error = std::abs(std::log(std::max(ratio, 1e-9)));
        if (error < bestError) {
            bestError = error;
            F_extr = std::move(Fo);
            O_extr = std::move(Oo);
            Nf_extr = std::move(Nfo);
            if (report)
                *report = r;
        }
        if (ratio >= 0.7 && ratio <= 1.45)
            break;
        if (attempt == attempts - 1 || got <= 0) {
            cout << "Face target: " << (uint64_t) F_extr.cols() << " faces for ~" << (uint64_t) std::round(target)
                 << " asked (" << (int) std::round(100.0 * F_extr.cols() / target) << "%), the nearest of "
                 << attempts << " attempts." << endl;
            break;
        }
        if (ratio < 1)
            under = std::max(under, (double) p.face_percent);
        else
            over = over < 0 ? (double) p.face_percent : std::min(over, (double) p.face_percent);
        /* the next target: between a short and a long attempt, else scaled by the gap (bounded) */
        if (under > 0 && over > 0)
            p.face_percent = (Float) std::sqrt(under * over);
        else
            p.face_percent = (Float) (p.face_percent * std::min(4.0, std::max(0.25, 1.0 / ratio)));
        cout << "Face target missed: " << (uint64_t) got << " faces for ~" << (uint64_t) std::round(target)
             << " asked (" << (int) std::round(100.0 * ratio) << "%: thin parts merged?), again at "
             << p.face_percent << "% .." << endl;
    }
}

/* ------------------------------------------------------------------------- */
/*  Batch modes                                                              */
/* ------------------------------------------------------------------------- */

ObjectResult remesh_object(SceneFile &scene, const std::string &path, const FaceTarget &target,
                           const RemeshParams &params) {
    MatrixXu F;
    MatrixXf V, N;
    uint64_t polygons = 0;
    const bool transfer = params.uv == RemeshParams::UVTransfer;
    std::vector<UVSet> uvs;
    scene.load(path, F, V, &polygons, transfer ? &uvs : nullptr);
    MatrixXu F0;
    MatrixXf V0;
    if (transfer) {
        F0 = F;
        V0 = V;
    }

    RemeshParams p = params;
    p.scale = -1;
    p.vertex_count = -1;
    p.face_percent = target.percent;
    p.face_count = target.count;
    ObjectResult r;
    MatrixXf Nf;
    remesh(F, V, N, polygons, p, r.F, r.V, Nf, &r.report);
    if (r.F.cols() == 0)
        throw std::runtime_error("Remeshing \"" + path + "\" produced no faces (target too small for this mesh?)");
    if (transfer)
        r.uvs = transfer_uvs(F0, V0, uvs, r.F, r.V, path);
    else if (params.uv == RemeshParams::UVUnwrap)
        r.uvs = unwrap(r.F, r.V);
    return r;
}

std::vector<CornerUVs> new_mesh_uvs(const MatrixXu &F0, const MatrixXf &V0, const std::vector<UVSet> &uvs0,
                                    const MatrixXu &F, const MatrixXf &V, RemeshParams::UVMode mode,
                                    const std::string &what) {
    if (mode == RemeshParams::UVUnwrap)
        return unwrap(F, V);
    if (mode == RemeshParams::UVTransfer)
        return transfer_uvs(F0, V0, uvs0, F, V, what);
    return std::vector<CornerUVs>();
}

std::vector<CornerUVs> object_uvs(SceneFile &scene, const std::string &path, const MatrixXu &F,
                                  const MatrixXf &V, RemeshParams::UVMode mode) {
    if (mode == RemeshParams::UVUnwrap)
        return unwrap(F, V);
    if (mode != RemeshParams::UVTransfer)
        return std::vector<CornerUVs>();
    MatrixXu F0;
    MatrixXf V0;
    std::vector<UVSet> uvs;
    scene.load(path, F0, V0, nullptr, &uvs);
    return transfer_uvs(F0, V0, uvs, F, V, path);
}

void batch_process(const std::string &input, const std::string &output,
                   const RemeshParams &params) {
    cout << endl;
    cout << "Running in batch mode:" << endl;
    cout << "   Input file             = " << input << endl;
    cout << "   Output file            = " << output << endl;
    print_settings(params);
    cout << endl;

    MatrixXu F, F_extr;
    MatrixXf V, N, O_extr, Nf_extr;

    /* Load the input mesh */
    uint64_t polygons = 0;
    const bool transfer = params.uv == RemeshParams::UVTransfer;
    std::vector<UVSet> uvs;
    SceneUnits units;
    load_mesh_or_pointcloud(input, F, V, N, ProgressCallback(), &polygons, transfer ? &uvs : nullptr, &units);

    /* remesh() consumes the input: the UV transfer works on a copy */
    MatrixXu F0;
    MatrixXf V0;
    if (transfer) {
        F0 = F;
        V0 = V;
    }
    remesh(F, V, N, polygons, params, F_extr, O_extr, Nf_extr);

    std::vector<CornerUVs> outUVs;
    if (transfer)
        outUVs = transfer_uvs(F0, V0, uvs, F_extr, O_extr, "\"" + input + "\"");
    else if (params.uv == RemeshParams::UVUnwrap)
        outUVs = unwrap(F_extr, O_extr);

    write_mesh(output, F_extr, O_extr, MatrixXf(), Nf_extr, MatrixXf(), MatrixXf(), ProgressCallback(), outUVs, units);
}

/* Polygon meshes of a scene file (Alembic meshes, OBJ objects) */
static std::vector<abc::MeshSummary> list_scene(const std::string &input) {
    return SceneFile::open(input)->meshes();
}

/* --progress: a block that stands out in a long log
   >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>
   >>> Progress  20%  [######------------------------]  3/15 meshes  elapsed ...
   >>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>>> */
static void print_progress(double fraction, size_t done, size_t total, double elapsedMs,
                           const std::string &last) {
    fraction = std::min(1.0, std::max(0.0, fraction));
    const int percent = done == total && total > 0 ? 100 : std::min(99, (int) std::floor(fraction * 100));
    const int width = 30, filled = (int) std::round(fraction * width);
    const std::string rule(74, '>');
    cout << endl << rule << endl
         << ">>> Progress " << std::setw(3) << percent << "%  ["
         << std::string((size_t) filled, '#') << std::string((size_t) (width - filled), '-') << "]  "
         << done << "/" << total << " meshes";
    if (done > 0) {
        cout << "  elapsed " << timeString(elapsedMs);
        if (done < total && fraction > 0)
            cout << ", ~" << timeString(elapsedMs * (1 - fraction) / fraction) << " left";
    }
    if (!last.empty())
        cout << "  (" << last << ")";
    cout << endl << rule << endl;
}

void batch_list(const std::string &input, int sort, int top) {
    const std::vector<abc::MeshSummary> all = list_scene(input);
    /* Sorted through pointers: MeshSummary holds an aligned Eigen matrix */
    std::vector<const abc::MeshSummary *> meshes;
    for (const abc::MeshSummary &m : all)
        meshes.push_back(&m);
    if (sort != 0)
        std::sort(meshes.begin(), meshes.end(),
                  [sort](const abc::MeshSummary *a, const abc::MeshSummary *b) {
                      if (a->faces != b->faces)
                          return sort > 0 ? a->faces < b->faces : a->faces > b->faces;
                      return a->path < b->path;
                  });
    if (top > 0 && (size_t) top < meshes.size())
        meshes.resize((size_t) top);

    size_t width = 0;
    for (const abc::MeshSummary *m : meshes)
        width = std::max(width, m->path.size());
    cout << "Polygon meshes in \"" << input << "\": " << all.size();
    if (sort != 0)
        cout << ", by face count " << (sort > 0 ? "(ascending)" : "(descending)");
    if (meshes.size() < all.size())
        cout << ", first " << meshes.size() << " shown";
    cout << endl;
    for (const abc::MeshSummary *m : meshes)
        cout << "   " << std::left << std::setw((int) width) << m->path << std::right
             << "  " << std::setw(9) << m->faces << " faces  " << std::setw(9) << m->vertices
             << " vertices" << mesh_flags(*m) << endl;
}

/* The plan of a project, the remeshing of its meshes that have a target
   and are not done yet, the scene written to 'output' (if any), the
   project saved to 'saveImd' (if any) */
static void run_project(Project &project, const std::string &label, const std::string &output, bool dryRun,
                        bool progress, const std::string &saveImd) {
    const bool proxy = project.options.proxy;
    const RemeshParams &params = project.options.params;

    /* --proxy: where each proxy goes, checked before any computation */
    std::map<std::string, std::string> proxyOf;
    if (proxy) {
        std::vector<std::string> targets;
        for (const ProjectObject &o : project.objects)
            if (project.target_of(o).valid())
                targets.push_back(o.mesh.path);
        const std::vector<std::string> where = project.scene().proxy_paths(targets);
        for (size_t k = 0; k < targets.size(); ++k)
            proxyOf[targets[k]] = where[k];
    }

    size_t width = 0, count = 0, todo = 0;
    for (const ProjectObject &o : project.objects) {
        width = std::max(width, o.mesh.path.size());
        if (project.target_of(o).valid()) {
            ++count;
            todo += o.state != ObjectState::Done;
        }
    }
    cout << endl << "Plan for \"" << label << "\" (" << count << " of " << project.objects.size()
         << (proxy ? " polygon meshes get a proxy):" : " polygon meshes remeshed):") << endl;
    for (const ProjectObject &o : project.objects) {
        const FaceTarget t = project.target_of(o);
        cout << "   " << std::left << std::setw((int) width) << o.mesh.path << std::right
             << "  " << std::setw(9) << o.mesh.faces << " faces";
        if (t.valid()) {
            cout << "  -> " << (proxy ? "proxy " : "") << t.text;
            if (t.percent > 0)
                cout << " (~" << (uint64_t) std::round(o.mesh.faces * t.percent / 100.0) << ")";
        }
        cout << "   [" << project.reason_of(o);
        if (t.valid() && o.state != ObjectState::Pending)
            cout << ", " << state_name(o.state);
        cout << "]" << endl;
        if (proxy && t.valid())
            cout << "   " << std::string(width, ' ') << "  proxy: " << proxyOf[o.mesh.path] << endl;
    }
    if (proxy && !output.empty() && usd::proxy_output_error(project.source, output).empty())
        cout << "The proxies go to \"" << usd::proxy_layer_path(project.source, output) << "\", referenced by \""
             << output << "\" (" << (str_tolower(output) == str_tolower(project.source) ? "the scene itself"
                                                                                       : "a copy of the scene")
             << ")" << endl;
    if (count == 0)
        throw std::runtime_error("Nothing to remesh!");
    if (dryRun) {
        if (!saveImd.empty()) {
            project.save(saveImd);
            cout << "Project saved: \"" << saveImd << "\"" << endl;
        }
        cout << "Dry run: nothing computed, nothing written." << endl;
        return;
    }

    cout << endl << "Running in batch mode:" << endl;
    cout << "   Input file             = " << label << endl;
    cout << "   Output file            = " << output << endl;
    print_settings(params);

    /* --progress: weighted by input faces, the remeshing time follows them */
    uint64_t totalFaces = 0, doneFaces = 0;
    size_t done = 0;
    for (const ProjectObject &o : project.objects)
        if (project.target_of(o).valid() && o.state != ObjectState::Done)
            totalFaces += o.mesh.faces;
    Timer<> clock;
    if (progress)
        print_progress(0, 0, todo, 0, "");

    std::vector<std::string> skipped, subdivided;
    for (ProjectObject &o : project.objects) {
        const FaceTarget t = project.target_of(o);
        if (!t.valid() || o.state == ObjectState::Done)
            continue;
        cout << endl << "=== " << o.mesh.path << " -> " << (proxy ? "proxy " : "") << t.text << endl;
        const size_t skippedBefore = skipped.size();
        try {
            const RemeshReport report = project.process(o);
            if (report.subdivided > report.triangles)
                subdivided.push_back(o.mesh.path + ": " + std::to_string(report.triangles) + " -> " +
                                     std::to_string(report.subdivided) + " triangles");
        } catch (const std::exception &e) {
            /* --skip-failed: keep this object as it is and go on */
            if (!project.options.skipFailed)
                throw;
            o.state = ObjectState::Skipped;
            cout << "Skipped, kept unchanged: " << e.what() << endl;
            skipped.push_back(o.mesh.path + ": " + e.what());
        }
        if (progress) {
            doneFaces += o.mesh.faces;
            ++done;
            const double fraction = totalFaces > 0 ? (double) doneFaces / totalFaces : (double) done / todo;
            print_progress(fraction, done, todo, (double) clock.value(),
                           o.mesh.path + (skipped.size() > skippedBefore ? " skipped" : " done"));
        }
    }

    cout << endl;
    if (!subdivided.empty()) {
        cout << "Input subdivided before remeshing (the heaviest to compute), " << subdivided.size()
             << " of " << todo << " meshes:" << endl;
        for (const std::string &s : subdivided)
            cout << "   " << s << endl;
        cout << endl;
    }
    if (!skipped.empty()) {
        cout << "Skipped " << skipped.size() << " of " << todo
             << " meshes (--skip-failed, copied unchanged):" << endl;
        for (const std::string &s : skipped)
            cout << "   " << s << endl;
        cout << endl;
    }
    if (!output.empty())
        project.write(output);
    if (!saveImd.empty()) {
        project.save(saveImd);
        cout << "Project saved: \"" << saveImd << "\"" << endl;
    }
}

void batch_process_objects(const std::string &input, const std::string &output,
                           const RemeshParams &params, const std::vector<MeshRule> &rules,
                           const FaceTarget &others, bool dryRun, bool skipFailed, bool progress,
                           bool proxy, const std::string &saveImd) {
    /* Opened once for the plan, the loads and the write (an OBJ file is
       parsed once) */
    std::unique_ptr<Project> project = Project::create(input);
    project->options.params = params;
    project->options.others = others;
    project->options.proxy = proxy;
    project->options.skipFailed = skipFailed;
    project->apply_rules(rules);
    /* The new meshes wait on disk for the final write, not in memory */
    project->set_spool((output.empty() ? saveImd : output) + ".spool.tmp");
    run_project(*project, input, output, dryRun, progress, saveImd);
}

void batch_process_project(const std::string &imd, const std::string &output, bool dryRun, bool progress) {
    std::unique_ptr<Project> project = Project::load(imd);
    /* the output of a scene: same format (Alembic, OBJ), a layer (USD) */
    auto ext = [](const std::string &f) {
        const size_t dot = f.rfind('.');
        return dot == std::string::npos ? std::string() : str_tolower(f.substr(dot));
    };
    const std::string in = ext(project->source), out = ext(output);
    if (!output.empty()) {
        const bool usd = in == ".usd" || in == ".usda" || in == ".usdc" || in == ".usdz";
        if (usd && out != ".usda" && out != ".usdc")
            throw std::runtime_error("The scene of this project is USD: write a .usda or .usdc layer over it!");
        if (!usd && out != in)
            throw std::runtime_error("The scene of this project is " + in + ": write a " + in + " file!");
    }
    project->set_spool((output.empty() ? imd : output) + ".spool.tmp");
    run_project(*project, imd, output, dryRun, progress, imd);
}
