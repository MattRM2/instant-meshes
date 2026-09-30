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
#include <iomanip>

/* ------------------------------------------------------------------------- */
/*  Face targets and mesh rules                                              */
/* ------------------------------------------------------------------------- */

FaceTarget parse_face_target(const std::string &text) {
    FaceTarget t;
    t.text = text;
    if (!text.empty() && text.back() == '%') {
        t.percent = str_to_float(text.substr(0, text.size() - 1));
        if (!std::isfinite(t.percent) || !(t.percent > 0))
            throw std::runtime_error("Invalid face percentage \"" + text + "\"");
    } else {
        t.count = str_to_int32_t(text);
        if (t.count <= 0)
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

static void print_settings(const RemeshParams &p) {
    cout << "   Rotation symmetry type = " << p.rosy << endl;
    cout << "   Position symmetry type = " << (p.posy==3?6:p.posy) << endl;
    cout << "   Crease angle threshold = ";
    if (p.crease_angle > 0)
        cout << p.crease_angle << endl;
    else
        cout << "disabled" << endl;
    cout << "   Extrinsic mode         = " << (p.extrinsic ? "enabled" : "disabled") << endl;
    cout << "   Align to boundaries    = " << (p.align_to_boundaries ? "yes" : "no") << endl;
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

void remesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
            const RemeshParams &params, MatrixXu &F_extr, MatrixXf &O_extr,
            MatrixXf &Nf_extr) {
    const int rosy = params.rosy, posy = params.posy;
    Float scale = params.scale, face_percent = params.face_percent;
    int face_count = params.face_count, vertex_count = params.vertex_count;
    const Float creaseAngle = params.crease_angle;
    const bool extrinsic = params.extrinsic, align_to_boundaries = params.align_to_boundaries;
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

    if (!pointcloud) {
        /* Subdivide the mesh if necessary */
        VectorXu V2E, E2E;
        VectorXb boundary, nonManifold;
        if (stats.mMaximumEdgeLength*2 > scale || stats.mMaximumEdgeLength > stats.mAverageEdgeLength * 2) {
            cout << "Input mesh is too coarse for the desired output edge length "
                    "(max input mesh edge length=" << stats.mMaximumEdgeLength
                 << "), subdividing .." << endl;
            build_dedge(F, V, V2E, E2E, boundary, nonManifold);
            subdivide(F, V, V2E, E2E, boundary, nonManifold, std::min(scale/2, (Float) stats.mAverageEdgeLength*2), deterministic);
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
}

/* ------------------------------------------------------------------------- */
/*  Batch modes                                                              */
/* ------------------------------------------------------------------------- */

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
    load_mesh_or_pointcloud(input, F, V, N, ProgressCallback(), &polygons);

    remesh(F, V, N, polygons, params, F_extr, O_extr, Nf_extr);

    write_mesh(output, F_extr, O_extr, MatrixXf(), Nf_extr);
}

static bool is_obj_file(const std::string &filename) {
    return filename.size() > 4 && str_tolower(filename.substr(filename.size() - 4)) == ".obj";
}

/* Polygon meshes of an Alembic file, or objects of an OBJ file ("/name") */
static std::vector<abc::MeshSummary> list_scene(const std::string &input) {
    if (!is_obj_file(input))
        return abc::list_meshes(input);
    std::vector<abc::MeshSummary> result;
    for (const objscene::ObjectInfo &o : objscene::list_objects(input)) {
        abc::MeshSummary m;
        m.path = "/" + o.name;
        m.vertices = o.vertices;
        m.faces = o.faces;
        result.push_back(m);
    }
    return result;
}

static std::string mesh_flags(const abc::MeshSummary &m) {
    std::string flags;
    if (m.animated)
        flags += " (animated)";
    if (m.instanced)
        flags += " (instanced)";
    return flags;
}

void batch_list(const std::string &input) {
    const std::vector<abc::MeshSummary> meshes = list_scene(input);
    size_t width = 0;
    for (const abc::MeshSummary &m : meshes)
        width = std::max(width, m.path.size());
    cout << "Polygon meshes in \"" << input << "\": " << meshes.size() << endl;
    for (const abc::MeshSummary &m : meshes)
        cout << "   " << std::left << std::setw((int) width) << m.path << std::right
             << "  " << std::setw(9) << m.faces << " faces  " << std::setw(9) << m.vertices
             << " vertices" << mesh_flags(m) << endl;
}

void batch_process_objects(const std::string &input, const std::string &output,
                           const RemeshParams &params, const std::vector<MeshRule> &rules,
                           const FaceTarget &others, bool dryRun, bool skipFailed) {
    const std::vector<abc::MeshSummary> meshes = list_scene(input);
    const bool obj = is_obj_file(input);

    /* Plan: the last matching rule wins, then --others, else untouched */
    struct Item {
        const abc::MeshSummary *mesh;
        FaceTarget target;
        std::string reason;
        bool remesh;
    };
    std::vector<Item> plan;
    std::vector<size_t> matches(rules.size(), 0);
    std::vector<std::string> refused;
    for (const abc::MeshSummary &m : meshes) {
        Item item { &m, FaceTarget(), "", false };
        int rule = -1;
        for (size_t r = 0; r < rules.size(); ++r) {
            if (rule_matches(rules[r].pattern, m.path)) {
                rule = (int) r;
                matches[r]++;
            }
        }
        if (rule >= 0) {
            item.target = rules[rule].target;
            item.reason = "-m " + rules[rule].text;
            item.remesh = true;
            if (m.animated || m.instanced)
                refused.push_back(m.path + mesh_flags(m) + ", selected by -m " + rules[rule].text);
        } else if (others.valid()) {
            if (m.animated || m.instanced) {
                item.reason = "kept unchanged" + mesh_flags(m);
            } else {
                item.target = others;
                item.reason = "--others " + others.text;
                item.remesh = true;
            }
        } else {
            item.reason = "kept unchanged";
        }
        plan.push_back(item);
    }

    for (size_t r = 0; r < rules.size(); ++r)
        if (matches[r] == 0)
            throw std::runtime_error("-m " + rules[r].text + ": no polygon mesh matches \"" +
                                     rules[r].pattern + "\" (see --list)");
    if (!refused.empty()) {
        std::string list;
        for (const std::string &s : refused)
            list += "\n   " + s;
        throw std::runtime_error("Animated or instanced meshes cannot be remeshed:" + list);
    }

    size_t width = 0, count = 0;
    for (const Item &i : plan) {
        width = std::max(width, i.mesh->path.size());
        count += i.remesh;
    }
    cout << endl << "Plan for \"" << input << "\" (" << count << " of " << plan.size()
         << " polygon meshes remeshed):" << endl;
    for (const Item &i : plan) {
        cout << "   " << std::left << std::setw((int) width) << i.mesh->path << std::right
             << "  " << std::setw(9) << i.mesh->faces << " faces";
        if (i.remesh) {
            cout << "  -> " << i.target.text;
            if (i.target.percent > 0)
                cout << " (~" << (uint64_t) std::round(i.mesh->faces * i.target.percent / 100.0) << ")";
        }
        cout << "   [" << i.reason << "]" << endl;
    }
    if (count == 0)
        throw std::runtime_error("Nothing to remesh!");
    if (dryRun) {
        cout << "Dry run: nothing computed, nothing written." << endl;
        return;
    }

    cout << endl << "Running in batch mode:" << endl;
    cout << "   Input file             = " << input << endl;
    cout << "   Output file            = " << output << endl;
    print_settings(params);

    std::vector<abc::Replacement> replacements;
    std::vector<std::string> skipped;
    for (const Item &i : plan) {
        if (!i.remesh)
            continue;
        cout << endl << "=== " << i.mesh->path << " -> " << i.target.text << endl;
        try {
            MatrixXu F;
            MatrixXf V, N;
            uint64_t polygons = 0;
            if (obj)
                objscene::load_object(input, i.mesh->path.substr(1), F, V, &polygons);
            else
                abc::load_abc_mesh(input, i.mesh->path, F, V, &polygons);

            RemeshParams p = params;
            p.scale = -1;
            p.vertex_count = -1;
            p.face_percent = i.target.percent;
            p.face_count = i.target.count;
            abc::Replacement r;
            r.path = i.mesh->path;
            MatrixXf Nf;
            remesh(F, V, N, polygons, p, r.F, r.V, Nf);
            if (r.F.cols() == 0)
                throw std::runtime_error("Remeshing \"" + r.path + "\" produced no faces "
                                         "(target too small for this mesh?)");
            replacements.push_back(std::move(r));
        } catch (const std::exception &e) {
            /* --skip-failed: keep this object as it is and go on */
            if (!skipFailed)
                throw;
            cout << "Skipped, kept unchanged: " << e.what() << endl;
            skipped.push_back(i.mesh->path + ": " + e.what());
        }
    }

    cout << endl;
    if (!skipped.empty()) {
        cout << "Skipped " << skipped.size() << " of " << count
             << " meshes (--skip-failed, copied unchanged):" << endl;
        for (const std::string &s : skipped)
            cout << "   " << s << endl;
        cout << endl;
    }
    if (obj) {
        std::vector<objscene::Replacement> objects;
        for (abc::Replacement &r : replacements) {
            objscene::Replacement o;
            o.name = r.path.substr(1);
            o.F = std::move(r.F);
            o.V = std::move(r.V);
            objects.push_back(std::move(o));
        }
        objscene::splice_obj(input, output, objects);
    } else {
        abc::splice_abc(input, output, replacements);
    }
}
