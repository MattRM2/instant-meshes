/*
    test_uv.cpp -- UV sets: reading from Alembic and OBJ (uv_sets case of
    the reference data, built by tests/make_reference_data.py)
*/

#include "test_common.h"
#include "meshio.h"
#include "abc.h"
#include "objscene.h"
#include "uvtransfer.h"
#include "uvunwrap.h"
#include "batch.h"
#include <array>
#include <algorithm>

static const UVSet *find_set(const std::vector<UVSet> &sets, const std::string &name) {
    for (const UVSet &s : sets)
        if (s.name == name)
            return &s;
    return nullptr;
}

static Vector2f corner_uv(const UVSet &set, size_t corner) {
    return set.values.col(set.corners[corner]);
}

static void test_read() {
    std::cout << "uv: reading Alembic and OBJ UV sets" << std::endl;
    const std::string abcFile = data_path("uv_sets.abc"), objFile = data_path("uv_sets.obj");

    /* Alembic: the active map in .geom/uv (named after sourceName), the
       other one in .arbGeomParams */
    MatrixXu Fa, Fo;
    MatrixXf Va, Vo;
    std::vector<UVSet> ua, uo;
    CHECK(error_of([&] { abc::load_abc_mesh(abcFile, "/Cylinder/Cylinder", Fa, Va, nullptr, &ua); }) == "");
    CHECK(ua.size() == 2 && find_set(ua, "UVMap") && find_set(ua, "Planar"));
    for (const UVSet &s : ua)
        CHECK(s.corners.size() == 3 * (size_t) Fa.cols());

    /* OBJ: the active map only ("vt"), same triangles as the Alembic twin */
    objscene::Scene scene(objFile);
    CHECK(error_of([&] { scene.load("Cylinder", Fo, Vo, nullptr, &uo); }) == "");
    CHECK(uo.size() == 1 && uo[0].name == "uv" && uo[0].corners.size() == 3 * (size_t) Fo.cols());
    CHECK(Fo == Fa && Vo.cols() == Va.cols() && (Vo - Va).cwiseAbs().maxCoeff() < 1e-5f);

    const UVSet *mainA = find_set(ua, "UVMap"), *planar = find_set(ua, "Planar");
    if (mainA && uo.size() == 1 && Fo == Fa) {
        float diff = 0;
        for (size_t c = 0; c < mainA->corners.size(); ++c)
            diff = std::max(diff, (corner_uv(*mainA, c) - corner_uv(uo[0], c)).cwiseAbs().maxCoeff());
        CHECK(diff < 1e-5f);
    }
    /* The seam: corners of the same vertex with different UVs */
    if (mainA) {
        bool seam = false;
        for (size_t c = 0; c < mainA->corners.size() && !seam; ++c)
            for (size_t e = c + 1; e < mainA->corners.size() && !seam; ++e)
                seam = Fa(c % 3, c / 3) == Fa(e % 3, e / 3) &&
                       (corner_uv(*mainA, c) - corner_uv(*mainA, e)).norm() > 0.1f;
        CHECK(seam);
    }
    /* Planar map: (0.5 + 0.4 x, 0.5 + 0.4 z) in Blender object space, that
       is (0.5 + 0.4 (X + 2), 0.5 + 0.4 Y) in the Y-up world of the file */
    if (planar) {
        float diff = 0;
        for (size_t c = 0; c < planar->corners.size(); ++c) {
            const Vector3f p = Va.col(Fa(c % 3, c / 3));
            const Vector2f want(0.5f + 0.4f * (p.x() + 2), 0.5f + 0.4f * p.y());
            diff = std::max(diff, (corner_uv(*planar, c) - want).cwiseAbs().maxCoeff());
        }
        CHECK(diff < 1e-5f);
    }

    /* A mesh without UVs, and a grid with the default map */
    MatrixXu F;
    MatrixXf V;
    std::vector<UVSet> u;
    abc::load_abc_mesh(abcFile, "/NoUV/NoUV", F, V, nullptr, &u);
    CHECK(u.empty());
    scene.load("NoUV", F, V, nullptr, &u);
    CHECK(u.empty());
    abc::load_abc_mesh(abcFile, "/Grid/Grid", F, V, nullptr, &u);
    CHECK(u.size() == 1 && u[0].name == "UVMap" && u[0].corners.size() == 3 * (size_t) F.cols());

    /* Whole file: a set is kept only if every mesh has it (NoUV has none) */
    MatrixXf N;
    load_mesh_or_pointcloud(objFile, F, V, N, ProgressCallback(), nullptr, &u);
    CHECK(u.empty());
    load_mesh_or_pointcloud(abcFile, F, V, N, ProgressCallback(), nullptr, &u);
    CHECK(u.empty());
    /* ... and asking for UVs does not change the mesh */
    MatrixXu F2;
    MatrixXf V2;
    load_mesh_or_pointcloud(abcFile, F2, V2, N);
    CHECK(F2 == F && V2 == V);

    /* A bad texture coordinate index only drops the UVs */
    const std::string bad = temp_path("bad_vt.obj");
    write_file(bad, std::string("v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nf 1/1 2/7 3/1\n"));
    CHECK(error_of([&] { load_obj(bad, F, V, ProgressCallback(), nullptr, &u); }) == "");
    CHECK(u.empty() && F.cols() == 1);
}

/* Every triangle corner as (x, y, z, u, v), rounded and sorted: two meshes
   with the same triangles and UVs give the same list, whatever their
   vertex numbering */
static std::vector<std::array<long long, 5>> corner_list(const MatrixXu &F, const MatrixXf &V, const UVSet &set) {
    std::vector<std::array<long long, 5>> result;
    for (size_t c = 0; c < set.corners.size(); ++c) {
        const Vector3f p = V.col(F(c % 3, c / 3));
        const Vector2f uv = set.values.col(set.corners[c]);
        result.push_back({ std::llround(p.x() * 1e5), std::llround(p.y() * 1e5), std::llround(p.z() * 1e5),
                           std::llround(uv.x() * 1e5), std::llround(uv.y() * 1e5) });
    }
    std::sort(result.begin(), result.end());
    return result;
}

static void test_write() {
    std::cout << "uv: writing UV sets (Alembic, OBJ, whole file and per object)" << std::endl;
    const std::string abcFile = data_path("uv_sets.abc"), objFile = data_path("uv_sets.obj");
    MatrixXu F;
    MatrixXf V;
    std::vector<UVSet> uvs;
    abc::load_abc_mesh(abcFile, "/Cylinder/Cylinder", F, V, nullptr, &uvs);
    if (uvs.size() != 2)
        return;
    std::vector<CornerUVs> corners { corner_uvs(uvs[0], F), corner_uvs(uvs[1], F) };
    auto same_sets = [&](const MatrixXu &F2, const MatrixXf &V2, const std::vector<UVSet> &got, size_t count) {
        bool ok = got.size() == count;
        for (size_t i = 0; ok && i < count; ++i)
            ok = got[i].name == uvs[i].name && corner_list(F2, V2, got[i]) == corner_list(F, V, uvs[i]);
        return ok;
    };

    /* Whole file, Alembic: .geom/uv + .arbGeomParams, valid hashes */
    const std::string outAbc = temp_path("uv_write.abc");
    CHECK(error_of([&] { write_mesh(outAbc, F, V, MatrixXf(), MatrixXf(), MatrixXf(), MatrixXf(),
                                    ProgressCallback(), corners); }) == "");
    MatrixXu F2;
    MatrixXf V2;
    std::vector<UVSet> got;
    CHECK(error_of([&] { abc::load_abc_mesh(outAbc, "/uv_write/uv_write", F2, V2, nullptr, &got); }) == "");
    CHECK(same_sets(F2, V2, got, 2));
    CHECK(error_of([&] { abc::verify_hashes(outAbc); }) == "");

    /* Whole file, OBJ: the first set */
    const std::string outObj = temp_path("uv_write.obj");
    CHECK(error_of([&] { write_mesh(outObj, F, V, MatrixXf(), MatrixXf(), MatrixXf(), MatrixXf(),
                                    ProgressCallback(), corners); }) == "");
    CHECK(error_of([&] { load_obj(outObj, F2, V2, ProgressCallback(), nullptr, &got); }) == "");
    CHECK(got.size() == 1 && corner_list(F2, V2, got[0]) == corner_list(F, V, uvs[0]));

    /* Per object, Alembic: the cylinder replaced by itself with its UVs;
       the grid keeps its own */
    abc::Replacement r;
    r.path = "/Cylinder/Cylinder";
    r.F = F;
    r.V = V;
    r.uvs = corners;
    const std::string spliced = temp_path("uv_splice.abc");
    CHECK(error_of([&] { abc::splice_abc(abcFile, spliced, { r }); }) == "");
    CHECK(error_of([&] { abc::verify_hashes(spliced); }) == "");
    CHECK(error_of([&] { abc::load_abc_mesh(spliced, "/Cylinder/Cylinder", F2, V2, nullptr, &got); }) == "");
    CHECK(same_sets(F2, V2, got, 2));
    MatrixXu Fg, Fg2;
    MatrixXf Vg, Vg2;
    std::vector<UVSet> ug, ug2;
    abc::load_abc_mesh(abcFile, "/Grid/Grid", Fg, Vg, nullptr, &ug);
    abc::load_abc_mesh(spliced, "/Grid/Grid", Fg2, Vg2, nullptr, &ug2);
    CHECK(ug.size() == 1 && ug2.size() == 1 && corner_list(Fg, Vg, ug[0]) == corner_list(Fg2, Vg2, ug2[0]));

    /* The same, fetched on demand (spool) */
    abc::Replacement lazy;
    lazy.path = r.path;
    lazy.fetch = [&](MatrixXu &Fo, MatrixXf &Vo, std::vector<CornerUVs> &uo) { Fo = F; Vo = V; uo = corners; };
    const std::string splicedLazy = temp_path("uv_splice_lazy.abc");
    CHECK(error_of([&] { abc::splice_abc(abcFile, splicedLazy, { lazy }); }) == "");
    CHECK(read_file(spliced) == read_file(splicedLazy));

    /* Per object, OBJ: new "vt" lines for the cylinder, the grid's kept */
    MatrixXu Fo;
    MatrixXf Vo;
    std::vector<UVSet> uo;
    objscene::Scene scene(objFile);
    scene.load("Cylinder", Fo, Vo, nullptr, &uo);
    objscene::Replacement ro;
    ro.name = "Cylinder";
    ro.F = Fo;
    ro.V = Vo;
    ro.uvs = { corner_uvs(uo[0], Fo) };
    const std::string splicedObj = temp_path("uv_splice.obj");
    CHECK(error_of([&] { scene.splice(splicedObj, { ro }); }) == "");
    objscene::Scene after(splicedObj);
    after.load("Cylinder", F2, V2, nullptr, &got);
    CHECK(got.size() == 1 && corner_list(F2, V2, got[0]) == corner_list(Fo, Vo, uo[0]));
    scene.load("Grid", Fg, Vg, nullptr, &ug);
    after.load("Grid", Fg2, Vg2, nullptr, &ug2);
    CHECK(ug.size() == 1 && ug2.size() == 1 && corner_list(Fg, Vg, ug[0]) == corner_list(Fg2, Vg2, ug2[0]));

    /* Irregular polygons (quad-dominant output): a pentagon stored as its
       directed edges, F(2) == F(3) = polygon id */
    MatrixXu Fp(4, 5);
    MatrixXf Vp(3, 6);
    Vp << 0, 1, 1.5f, 1, 0, 0.5f,
          0, 0, 0.8f, 1.6f, 1, 0.8f,
          0, 0, 0, 0, 0, 0;
    for (uint32_t k = 0; k < 5; ++k)
        Fp.col(k) = Vector4u(k, (k + 1) % 5, 5, 5);   /* vertex 5: the polygon id (unused) */
    CornerUVs cp;
    cp.name = "UVMap";
    cp.corners.setZero(2, 20);
    for (uint32_t k = 0; k < 5; ++k)
        cp.corners.col(4 * k) = Vector2f(0.1f * k, 0.2f * k);   /* corner 0 of column k = vertex k */
    for (const std::string &ext : { std::string(".obj"), std::string(".abc") }) {
        const std::string out = temp_path("uv_pentagon" + ext);
        CHECK(error_of([&] { write_mesh(out, Fp, Vp, MatrixXf(), MatrixXf(), MatrixXf(), MatrixXf(),
                                        ProgressCallback(), { cp }); }) == "");
        MatrixXu Fr;
        MatrixXf Vr, N;
        std::vector<UVSet> ur;
        load_mesh_or_pointcloud(out, Fr, Vr, N, ProgressCallback(), nullptr, &ur);
        bool ok = ur.size() == 1 && Fr.cols() == 3;
        for (size_t c = 0; ok && c < ur[0].corners.size(); ++c) {
            const Vector3f p = Vr.col(Fr(c % 3, c / 3));
            int k = -1;
            for (int i = 0; i < 5; ++i)
                if ((Vp.col(i) - p).norm() < 1e-6f)
                    k = i;
            ok = k >= 0 && (corner_uv(ur[0], c) - Vector2f(0.1f * k, 0.2f * k)).norm() < 1e-6f;
        }
        CHECK(ok);
    }
}

/* Distance from p to a triangle mesh (brute force, independent of the
   transfer code) */
static Float distance_to(const MatrixXu &F, const MatrixXf &V, const Vector3f &p) {
    auto segment = [](const Vector3f &p, const Vector3f &a, const Vector3f &b) {
        const Vector3f ab = b - a;
        const Float t = std::min((Float) 1, std::max((Float) 0, (p - a).dot(ab) / std::max(ab.squaredNorm(), (Float) 1e-30)));
        return (a + ab * t - p).norm();
    };
    Float best = std::numeric_limits<Float>::infinity();
    for (uint32_t f = 0; f < F.cols(); ++f) {
        const Vector3f a = V.col(F(0, f)), b = V.col(F(1, f)), c = V.col(F(2, f));
        const Vector3f n = (b - a).cross(c - a);
        Float dist = std::min(segment(p, a, b), std::min(segment(p, b, c), segment(p, c, a)));
        if (n.squaredNorm() > 0) {
            const Vector3f q = p - n * (n.dot(p - a) / n.squaredNorm());
            if (n.dot((b - a).cross(q - a)) >= 0 && n.dot((c - b).cross(q - b)) >= 0 &&
                n.dot((a - c).cross(q - c)) >= 0)
                dist = (q - p).norm();
        }
        best = std::min(best, dist);
    }
    return best;
}

/* The cylinder of uv_sets, remeshed (quads, then triangles) */
static void remeshed_cylinder(MatrixXu &F, MatrixXf &V, std::vector<UVSet> &uvs, MatrixXu &Fr, MatrixXf &Or,
                              int posy) {
    abc::load_abc_mesh(data_path("uv_sets.abc"), "/Cylinder/Cylinder", F, V, nullptr, &uvs);
    MatrixXu Fc = F;
    MatrixXf Vc = V, N, Nf;
    RemeshParams p;
    p.deterministic = true;
    p.face_count = 400;
    p.posy = posy;
    p.rosy = posy == 4 ? 4 : 6;
    remesh(Fc, Vc, N, 0, p, Fr, Or, Nf);
}

static void test_transfer() {
    std::cout << "uv: transfer (identity, planar map, cylindrical map with a seam)" << std::endl;
    MatrixXu F;
    MatrixXf V;
    std::vector<UVSet> uvs;
    abc::load_abc_mesh(data_path("uv_sets.abc"), "/Cylinder/Cylinder", F, V, nullptr, &uvs);
    if (uvs.size() != 2)
        return;

    /* Onto the original itself: the very same UVs, seams included */
    {
        UVTransfer t(F, V, uvs);
        UVTransfer::Stats stats;
        std::vector<CornerUVs> out = t.transfer(F, V, &stats);
        CHECK(out.size() == 2 && out[0].name == "UVMap" && out[1].name == "Planar");
        float diff = 0;
        for (size_t s = 0; s < out.size() && s < 2; ++s)
            for (size_t c = 0; c < uvs[s].corners.size(); ++c)
                diff = std::max(diff, (out[s].corners.col(c) - corner_uv(uvs[s], c)).cwiseAbs().maxCoeff());
        CHECK(diff < 1e-5f && stats.corners == 3 * (size_t) F.cols() && stats.flipped == 0);
    }

    for (int posy : { 4, 3 }) {
        MatrixXu Fr;
        MatrixXf Or;
        remeshed_cylinder(F, V, uvs, Fr, Or, posy);
        UVTransfer t(F, V, uvs);
        UVTransfer::Stats stats;
        std::vector<CornerUVs> out = t.transfer(Fr, Or, &stats);
        CHECK(out.size() == 2 && Fr.cols() > 100);
        if (out.size() != 2)
            continue;
        std::vector<uint32_t> sizes, indices, faceIds, cornerIds;
        extracted_polygons(Fr, sizes, indices, faceIds, &cornerIds);

        /* Planar map: linear in the position (slope 0.4), one island -> the
           formula at the nearest surface point, which is at most 'dist' away
           from the new vertex (rounded edges of the remesh) */
        float planar = 0;
        std::vector<Float> dist(Or.cols());
        for (uint32_t v = 0; v < Or.cols(); ++v)
            dist[v] = distance_to(F, V, Or.col(v));
        for (size_t c = 0; c < indices.size(); ++c) {
            const Vector3f p = Or.col(indices[c]);
            const Vector2f want(0.5f + 0.4f * (p.x() + 2), 0.5f + 0.4f * p.y());
            if (std::abs(p.y()) > 0.9f)
                continue;   /* the rim: a side face keeps to the side triangles (facing first) */
            const float err = (out[1].corners.col(cornerIds[c]) - want).cwiseAbs().maxCoeff();
            planar = std::max(planar, err - 0.4f * dist[indices[c]]);
        }
        CHECK(planar < 1e-3f);

        /* Cylindrical map: u = 0.5 + 0.5 a / 2pi around the axis, the seam
           at a = 0. Side faces: u follows the angle unwrapped around the
           face centre (past the seam: extended, not wrapped), v = 0.25 +
           0.25 Y; no face spans the texture */
        float side = 0, spread = 0;
        size_t sideFaces = 0, offset = 0;
        for (size_t k = 0; k < sizes.size(); offset += sizes[k], ++k) {
            Vector3f centre = Vector3f::Zero(), normal = Vector3f::Zero();
            for (uint32_t i = 0; i < sizes[k]; ++i)
                centre += Or.col(indices[offset + i]);
            centre /= (Float) sizes[k];
            for (uint32_t i = 0; i < sizes[k]; ++i)
                normal += Vector3f(Or.col(indices[offset + i])).cross(Vector3f(Or.col(indices[offset + (i + 1) % sizes[k]])));
            if (std::abs(normal.normalized().y()) > 0.3f || std::abs(centre.y()) > 0.85f)
                continue;   /* caps, and the rim where caps and side meet */
            ++sideFaces;
            /* angle in Blender object space: (x, y) = (X + 2, -Z) */
            auto angle = [](const Vector3f &p) { return std::atan2(-p.z(), p.x() + 2); };
            Float ac = angle(centre);
            if (ac < 0)
                ac += 2 * (Float) M_PI;
            /* Near the seam a face may be on either end of the strip: u, or
               u +- 0.5, but the same shift for all its corners */
            float umin = 10, umax = -10, faceErr = 10;
            for (int shift = -1; shift <= 1; ++shift) {
                float err = 0;
                for (uint32_t i = 0; i < sizes[k]; ++i) {
                    const Vector3f p = Or.col(indices[offset + i]);
                    Float a = angle(p);
                    while (a < ac - (Float) M_PI)
                        a += 2 * (Float) M_PI;
                    while (a > ac + (Float) M_PI)
                        a -= 2 * (Float) M_PI;
                    const Vector2f uv = out[0].corners.col(cornerIds[offset + i]);
                    const Vector2f want(0.5f + 0.5f * a / (2 * (Float) M_PI) + 0.5f * shift, 0.25f + 0.25f * p.y());
                    /* slopes: 0.5 / 2pi per unit of arc, 0.25 per unit of height */
                    err = std::max(err, (uv - want).cwiseAbs().maxCoeff() - 0.25f * dist[indices[offset + i]]);
                    if (shift == 0) {
                        umin = std::min(umin, uv.x());
                        umax = std::max(umax, uv.x());
                    }
                }
                faceErr = std::min(faceErr, err);
            }
            side = std::max(side, faceErr);
            spread = std::max(spread, umax - umin);
        }
        CHECK(sideFaces > 20 && side < 0.01f && spread < 0.25f);
        CHECK(stats.extended > 0);   /* the faces across the seam */
    }
}

/* Checks an unwrapped UV set: inside [0, 1], every face wound the same way
   in UV (the mesh is consistently oriented: no mirrored chart), no two
   faces covering the same texels */
static void check_unwrap(const MatrixXu &F, const CornerUVs &set, const std::string &label) {
    std::vector<uint32_t> sizes, indices, faceIds, cornerIds;
    extracted_polygons(F, sizes, indices, faceIds, &cornerIds);
    const int R = 512;
    std::vector<int> owner(R * R, -1);
    size_t overlaps = 0, covered = 0, positive = 0, negative = 0, outside = 0, offset = 0;
    for (size_t k = 0; k < sizes.size(); offset += sizes[k], ++k) {
        std::vector<Vector2f> uv(sizes[k]);
        for (uint32_t i = 0; i < sizes[k]; ++i) {
            uv[i] = set.corners.col(cornerIds[offset + i]);
            outside += !(uv[i].x() >= -1e-4f && uv[i].x() <= 1 + 1e-4f && uv[i].y() >= -1e-4f && uv[i].y() <= 1 + 1e-4f);
        }
        double area = 0;
        for (uint32_t i = 0; i < sizes[k]; ++i)
            area += (double) uv[i].x() * uv[(i + 1) % sizes[k]].y() - (double) uv[(i + 1) % sizes[k]].x() * uv[i].y();
        positive += area > 0;
        negative += area < 0;
        /* rasterize the fan at texel centres */
        for (uint32_t i = 1; i + 1 < sizes[k]; ++i) {
            const Vector2f a = uv[0] * R, b = uv[i] * R, c = uv[i + 1] * R;
            auto edge = [](const Vector2f &p, const Vector2f &q, const Vector2f &r) {
                return (q.x() - p.x()) * (r.y() - p.y()) - (q.y() - p.y()) * (r.x() - p.x());
            };
            const float w = edge(a, b, c);
            if (w == 0)
                continue;
            const int x0 = std::max(0, (int) std::floor(std::min(a.x(), std::min(b.x(), c.x()))));
            const int x1 = std::min(R - 1, (int) std::ceil(std::max(a.x(), std::max(b.x(), c.x()))));
            const int y0 = std::max(0, (int) std::floor(std::min(a.y(), std::min(b.y(), c.y()))));
            const int y1 = std::min(R - 1, (int) std::ceil(std::max(a.y(), std::max(b.y(), c.y()))));
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const Vector2f p(x + 0.5f, y + 0.5f);
                    if (edge(b, c, p) / w < 0 || edge(c, a, p) / w < 0 || edge(a, b, p) / w < 0)
                        continue;
                    int &o = owner[y * R + x];
                    if (o == -1) {
                        o = (int) k;
                        ++covered;
                    } else if (o != (int) k) {
                        ++overlaps;
                    }
                }
        }
    }
    std::cout << "  " << label << ": " << sizes.size() << " faces, " << covered * 100 / (R * R) << "% covered, "
              << overlaps << " overlapping texels, winding +" << positive << " -" << negative << ", "
              << outside << " outside" << std::endl;
    CHECK(outside == 0 && covered > (size_t) (R * R / 5));
    CHECK(std::min(positive, negative) == 0);
    CHECK(overlaps <= covered / 1000);   /* texel-centre ties on shared edges only */
}

static void test_unwrap() {
    std::cout << "uv: unwrap (xatlas, polygons kept whole, no mirrored chart)" << std::endl;
    for (int posy : { 4, 3 }) {
        MatrixXu F, Fr;
        MatrixXf V, Or;
        std::vector<UVSet> uvs;
        remeshed_cylinder(F, V, uvs, Fr, Or, posy);
        UnwrapStats stats;
        const CornerUVs set = unwrap_uvs(Fr, Or, "UVMap", &stats);
        CHECK(set.name == "UVMap" && set.corners.cols() == Fr.rows() * Fr.cols() && stats.charts > 0);
        check_unwrap(Fr, set, posy == 4 ? "quads" : "triangles");
        /* the same result every time */
        CHECK(unwrap_uvs(Fr, Or).corners == set.corners);
        /* a tiny object (triangle areas below FLT_EPSILON, that xatlas
           would drop): the same charts, the same UVs */
        const MatrixXf tiny = (Or * 1e-5f).eval();
        UnwrapStats tinyStats;
        const CornerUVs tinyUVs = unwrap_uvs(Fr, tiny, "UVMap", &tinyStats);
        CHECK(tinyStats.charts == stats.charts && (tinyUVs.corners - set.corners).cwiseAbs().maxCoeff() < 1e-3f);
    }
    /* Quad-dominant output: irregular polygons */
    MatrixXu F, Fr;
    MatrixXf V, Or, N, Nf;
    std::vector<UVSet> uvs;
    abc::load_abc_mesh(data_path("uv_sets.abc"), "/Cylinder/Cylinder", F, V, nullptr, &uvs);
    RemeshParams p;
    p.deterministic = true;
    p.face_count = 300;
    p.pure_quad = false;
    remesh(F, V, N, 0, p, Fr, Or, Nf);
    check_unwrap(Fr, unwrap_uvs(Fr, Or), "quad-dominant");
}

void test_uv() {
    test_read();
    test_write();
    test_transfer();
    test_unwrap();
}
