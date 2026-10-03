/*
    test_uv.cpp -- UV sets: reading from Alembic and OBJ (uv_sets case of
    the reference data, built by tests/make_reference_data.py)
*/

#include "test_common.h"
#include "meshio.h"
#include "abc.h"
#include "objscene.h"
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

void test_uv() {
    test_read();
    test_write();
}
