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
#include "uvtransfer.h"
#include "uvunwrap.h"
#include <iomanip>
#include <fstream>
#include <functional>
#include <memory>

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

void remesh(MatrixXu &F, MatrixXf &V, MatrixXf &N, uint64_t polygons,
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

/* Remeshed meshes waiting for the final write, kept in a temporary file
   (removed at the end, whatever happens) rather than in memory */
class Spool {
public:
    explicit Spool(const std::string &path) : mPath(path) { }
    ~Spool() {
        if (mOut.is_open())
            mOut.close();
        if (mCreated)
            std::remove(mPath.c_str());
    }

    typedef std::function<void(MatrixXu &, MatrixXf &, std::vector<CornerUVs> &)> Fetch;

    /* Stores a mesh and its UV sets, returns the function that reads them back */
    Fetch put(const MatrixXu &F, const MatrixXf &V, const std::vector<CornerUVs> &uvs) {
        if (!mOut.is_open()) {
            mOut.open(mPath, std::ios::binary | std::ios::trunc);
            if (!mOut)
                throw std::runtime_error("Unable to create \"" + mPath + "\"!");
            mCreated = true;
        }
        Record r { mSize, (uint32_t) F.rows(), (uint64_t) F.cols(), (uint64_t) V.cols(), {} };
        write(F.data(), sizeof(MatrixXu::Scalar) * (size_t) F.size());
        write(V.data(), sizeof(MatrixXf::Scalar) * (size_t) V.size());
        for (const CornerUVs &set : uvs) {
            r.uvNames.push_back(set.name);
            r.uvCorners.push_back((uint64_t) set.corners.cols());
            write(set.corners.data(), sizeof(MatrixXf::Scalar) * (size_t) set.corners.size());
        }
        return [this, r](MatrixXu &Fo, MatrixXf &Vo, std::vector<CornerUVs> &uvo) { get(r, Fo, Vo, uvo); };
    }

private:
    struct Record {
        uint64_t offset;
        uint32_t rows;
        uint64_t faces, vertices;
        std::vector<std::string> uvNames;
        std::vector<uint64_t> uvCorners;
    };

    void write(const void *data, size_t size) {
        mOut.write((const char *) data, (std::streamsize) size);
        if (!mOut)
            throw std::runtime_error("Error while writing \"" + mPath + "\" (disk full?)!");
        mSize += size;
    }

    void get(const Record &r, MatrixXu &F, MatrixXf &V, std::vector<CornerUVs> &uvs) {
        mOut.flush();
        std::ifstream in(mPath, std::ios::binary);
        in.seekg((std::streamoff) r.offset);
        F.resize(r.rows, (std::ptrdiff_t) r.faces);
        V.resize(3, (std::ptrdiff_t) r.vertices);
        in.read((char *) F.data(), (std::streamsize) (sizeof(MatrixXu::Scalar) * (size_t) F.size()));
        in.read((char *) V.data(), (std::streamsize) (sizeof(MatrixXf::Scalar) * (size_t) V.size()));
        uvs.resize(r.uvNames.size());
        for (size_t i = 0; i < uvs.size(); ++i) {
            uvs[i].name = r.uvNames[i];
            uvs[i].corners.resize(2, (std::ptrdiff_t) r.uvCorners[i]);
            in.read((char *) uvs[i].corners.data(),
                    (std::streamsize) (sizeof(MatrixXf::Scalar) * (size_t) uvs[i].corners.size()));
        }
        if (!in)
            throw std::runtime_error("Unable to read back \"" + mPath + "\"!");
    }

    std::string mPath;
    std::ofstream mOut;
    bool mCreated = false;
    uint64_t mSize = 0;
};

static std::string mesh_flags(const abc::MeshSummary &m) {
    std::string flags;
    if (m.animated)
        flags += " (animated)";
    if (m.instanced)
        flags += " (instanced)";
    if (!m.purpose.empty())
        flags += " (" + m.purpose + ")";
    return flags;
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

void batch_process_objects(const std::string &input, const std::string &output,
                           const RemeshParams &params, const std::vector<MeshRule> &rules,
                           const FaceTarget &others, bool dryRun, bool skipFailed, bool progress,
                           bool proxy) {
    /* Opened once for the plan, the loads and the write (an OBJ file is
       parsed once) */
    const std::unique_ptr<SceneFile> scene = SceneFile::open(input);
    const std::vector<abc::MeshSummary> meshes = scene->meshes();

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
    /* --proxy: proxy and guide meshes do not get one */
    auto unfit = [proxy](const abc::MeshSummary &m) {
        return m.animated || m.instanced || (proxy && (m.purpose == "proxy" || m.purpose == "guide"));
    };
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
            if (unfit(m))
                refused.push_back(m.path + mesh_flags(m) + ", selected by -m " + rules[rule].text);
        } else if (others.valid()) {
            if (unfit(m)) {
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
        throw std::runtime_error(proxy ? "Animated, instanced, proxy or guide meshes cannot get a proxy:" + list
                                       : "Animated or instanced meshes cannot be remeshed:" + list);
    }

    /* --proxy: where each proxy goes, checked before any computation */
    std::map<std::string, std::string> proxyOf;
    if (proxy) {
        std::vector<std::string> targets;
        for (const Item &i : plan)
            if (i.remesh)
                targets.push_back(i.mesh->path);
        const std::vector<std::string> where = scene->proxy_paths(targets);
        for (size_t k = 0; k < targets.size(); ++k)
            proxyOf[targets[k]] = where[k];
    }

    size_t width = 0, count = 0;
    for (const Item &i : plan) {
        width = std::max(width, i.mesh->path.size());
        count += i.remesh;
    }
    cout << endl << "Plan for \"" << input << "\" (" << count << " of " << plan.size()
         << (proxy ? " polygon meshes get a proxy):" : " polygon meshes remeshed):") << endl;
    for (const Item &i : plan) {
        cout << "   " << std::left << std::setw((int) width) << i.mesh->path << std::right
             << "  " << std::setw(9) << i.mesh->faces << " faces";
        if (i.remesh) {
            cout << "  -> " << (proxy ? "proxy " : "") << i.target.text;
            if (i.target.percent > 0)
                cout << " (~" << (uint64_t) std::round(i.mesh->faces * i.target.percent / 100.0) << ")";
        }
        cout << "   [" << i.reason << "]" << endl;
        if (proxy && i.remesh)
            cout << "   " << std::string(width, ' ') << "  proxy: " << proxyOf[i.mesh->path] << endl;
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

    /* --progress: weighted by input faces, the remeshing time follows them */
    uint64_t totalFaces = 0, doneFaces = 0;
    size_t done = 0;
    for (const Item &i : plan)
        if (i.remesh)
            totalFaces += i.mesh->faces;
    Timer<> clock;
    if (progress)
        print_progress(0, 0, count, 0, "");

    /* The new meshes wait on disk for the final write, not in memory */
    Spool spool(output + ".spool.tmp");
    std::vector<abc::Replacement> replacements;
    std::vector<std::string> skipped, subdivided;
    for (const Item &i : plan) {
        if (!i.remesh)
            continue;
        cout << endl << "=== " << i.mesh->path << " -> " << (proxy ? "proxy " : "") << i.target.text << endl;
        const size_t skippedBefore = skipped.size();
        try {
            MatrixXu F;
            MatrixXf V, N;
            uint64_t polygons = 0;
            const bool transfer = params.uv == RemeshParams::UVTransfer;
            std::vector<UVSet> uvs;
            scene->load(i.mesh->path, F, V, &polygons, transfer ? &uvs : nullptr);
            MatrixXu F0;
            MatrixXf V0;
            if (transfer) {
                F0 = F;
                V0 = V;
            }

            RemeshParams p = params;
            p.scale = -1;
            p.vertex_count = -1;
            p.face_percent = i.target.percent;
            p.face_count = i.target.count;
            MatrixXu Fr;
            MatrixXf Vr, Nf;
            RemeshReport report;
            remesh(F, V, N, polygons, p, Fr, Vr, Nf, &report);
            if (Fr.cols() == 0)
                throw std::runtime_error("Remeshing \"" + i.mesh->path + "\" produced no faces "
                                         "(target too small for this mesh?)");
            if (report.subdivided > report.triangles)
                subdivided.push_back(i.mesh->path + ": " + std::to_string(report.triangles) + " -> " +
                                     std::to_string(report.subdivided) + " triangles");
            abc::Replacement r;
            r.path = i.mesh->path;
            std::vector<CornerUVs> outUVs;
            if (transfer) {
                outUVs = transfer_uvs(F0, V0, uvs, Fr, Vr, i.mesh->path);
                F0.resize(0, 0);
                V0.resize(0, 0);
            } else if (params.uv == RemeshParams::UVUnwrap) {
                outUVs = unwrap(Fr, Vr);
            }
            r.fetch = spool.put(Fr, Vr, outUVs);
            replacements.push_back(std::move(r));
        } catch (const std::exception &e) {
            /* --skip-failed: keep this object as it is and go on */
            if (!skipFailed)
                throw;
            cout << "Skipped, kept unchanged: " << e.what() << endl;
            skipped.push_back(i.mesh->path + ": " + e.what());
        }
        if (progress) {
            doneFaces += i.mesh->faces;
            ++done;
            const double fraction = totalFaces > 0 ? (double) doneFaces / totalFaces : (double) done / count;
            print_progress(fraction, done, count, (double) clock.value(),
                           i.mesh->path + (skipped.size() > skippedBefore ? " skipped" : " done"));
        }
    }

    cout << endl;
    if (!subdivided.empty()) {
        cout << "Input subdivided before remeshing (the heaviest to compute), " << subdivided.size()
             << " of " << count << " meshes:" << endl;
        for (const std::string &s : subdivided)
            cout << "   " << s << endl;
        cout << endl;
    }
    if (!skipped.empty()) {
        cout << "Skipped " << skipped.size() << " of " << count
             << " meshes (--skip-failed, copied unchanged):" << endl;
        for (const std::string &s : skipped)
            cout << "   " << s << endl;
        cout << endl;
    }
    if (proxy) {
        if (replacements.empty())
            throw std::runtime_error("No proxy could be made, nothing written!");
        scene->write_proxies(output, replacements);
    } else {
        scene->write(output, replacements);
    }
}
