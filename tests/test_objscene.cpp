/*
    test_objscene.cpp -- Unit tests for per-object OBJ access and splicing
    (objscene.h): listing, loading, replacing objects while every other
    object comes back identical.
*/

#include "test_common.h"
#include "objscene.h"
#include "meshio.h"
#include "abc.h"

/* A quad grid spanning the bounding box of 'around' (extracted-mesh layout) */
static void make_grid(const MatrixXf &around, MatrixXu &F, MatrixXf &V, int n = 5) {
    const Vector3f lo = around.rowwise().minCoeff(), hi = around.rowwise().maxCoeff();
    V.resize(3, (n + 1) * (n + 1));
    F.resize(4, n * n);
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i)
            V.col(j * (n + 1) + i) = Vector3f(lo.x() + (hi.x() - lo.x()) * i / n,
                                              lo.y() + (hi.y() - lo.y()) * j / n,
                                              0.5f * (lo.z() + hi.z()) + 0.05f * i);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i)
            F.col(j * n + i) = Vector4u(j * (n + 1) + i, j * (n + 1) + i + 1,
                                        (j + 1) * (n + 1) + i + 1, (j + 1) * (n + 1) + i);
}

static std::string read_text(const std::string &path) {
    std::vector<uint8_t> b = read_file(path);
    return std::string(b.begin(), b.end());
}

static bool same_mesh(const std::string &a, const std::string &b, const std::string &name) {
    MatrixXu Fa, Fb;
    MatrixXf Va, Vb;
    objscene::load_object(a, name, Fa, Va);
    objscene::load_object(b, name, Fb, Vb);
    return Fa == Fb && Va == Vb;
}

static void test_blender_scene() {
    std::cout << "objscene: Blender scene (list, load, splice)" << std::endl;
    const std::string in = data_path("scene_ab.obj");

    std::vector<objscene::ObjectInfo> objects;
    CHECK(error_of([&] { objects = objscene::list_objects(in); }) == "");
    CHECK(objects.size() == 2 && objects[0].name == "MeshA" && objects[0].faces == 7872 &&
          objects[0].vertices == 7958 && objects[1].name == "MeshB" && objects[1].faces == 576);

    /* Same triangles as the Alembic twin */
    MatrixXu Fo, Fa;
    MatrixXf Vo, Va;
    objscene::load_object(in, "MeshA", Fo, Vo);
    abc::load_abc_mesh(data_path("scene_ab.abc"), "/Props/MeshA/MeshA", Fa, Va);
    CHECK(Fo == Fa && Vo.cols() == Va.cols() && (Vo - Va).cwiseAbs().maxCoeff() < 1e-5f);

    /* Replace MeshA: MeshB identical, its vertex lines copied as text */
    objscene::Replacement r;
    r.name = "MeshA";
    make_grid(Vo, r.F, r.V);
    const std::string out = temp_path("splice_scene_ab.obj");
    CHECK(error_of([&] { objscene::splice_obj(in, out, { r }); }) == "");
    CHECK(!file_exists(out + ".tmp"));
    CHECK(same_mesh(in, out, "MeshB"));

    MatrixXu Fn;
    MatrixXf Vn;
    CHECK(error_of([&] { objscene::load_object(out, "MeshA", Fn, Vn); }) == "");
    CHECK(Vn.cols() == 36 && Fn.cols() == 50);
    const std::string before = read_text(in), after = read_text(out);
    const size_t b0 = before.find("o MeshB"), a0 = after.find("o MeshB");
    CHECK(b0 != std::string::npos && a0 != std::string::npos);
    /* first vertex line of MeshB, verbatim */
    CHECK(after.find(before.substr(before.find("\nv ", b0), 30)) != std::string::npos);

    objects = objscene::list_objects(out);
    CHECK(objects.size() == 2 && objects[0].name == "MeshA" && objects[0].faces == 25 &&
          objects[1].name == "MeshB" && objects[1].faces == 576);
}

static const char *SYNTHETIC =
    "# synthetic scene\n"
    "mtllib test.mtl\n"
    "v 0 0 0\n"
    "v 1 0 0\n"
    "v 1 1 0\n"
    "v 0 1 0\n"
    "v 2 0 0\n"
    "v 2 1 0\n"
    "vt 0 0\n"
    "vt 1 0\n"
    "vt 1 1\n"
    "vn 0 0 1\n"
    "g A\n"
    "usemtl red\n"
    "s 1\n"
    "f 1/1/1 2/2/1 3/3/1 4/1/1\n"
    "g B\n"
    "f -5/1/1 -2/2/1 -1/3/1 -4/1/1\n"
    "l 5 6\n"
    "g C\n"
    "usemtl blue\n"
    "f 5 6 3\n";

static void test_synthetic() {
    std::cout << "objscene: groups, shared vertices, negative indices, materials" << std::endl;
    const std::string in = temp_path("synthetic.obj");
    write_file(in, std::string(SYNTHETIC));

    std::vector<objscene::ObjectInfo> objects = objscene::list_objects(in);
    CHECK(objects.size() == 3 && objects[0].name == "A" && objects[1].name == "B" &&
          objects[2].name == "C" && objects[1].vertices == 4);

    /* Negative indices: B is the quad 2 5 6 3 */
    MatrixXu F;
    MatrixXf V;
    objscene::load_object(in, "B", F, V);
    CHECK(F.cols() == 2 && V.cols() == 4 && V.col(0) == Vector3f(1, 0, 0) && V.col(1) == Vector3f(2, 0, 0));
    /* The main OBJ reader accepts them too */
    CHECK(error_of([&] { load_obj(in, F, V); }) == "" && F.cols() == 5);

    /* Replace A (vertices 1 and 4 are its own, 2 and 3 shared with B / C) */
    MatrixXf around(3, 2);
    around << 0, 1, 0, 1, 0, 0;
    objscene::Replacement r;
    r.name = "A";
    make_grid(around, r.F, r.V, 2);
    const std::string out = temp_path("synthetic_a.obj");
    CHECK(error_of([&] { objscene::splice_obj(in, out, { r }); }) == "");
    CHECK(same_mesh(in, out, "B") && same_mesh(in, out, "C"));
    const std::string text = read_text(out);
    size_t vlines = 0;
    for (size_t p = text.find("\nv "); p != std::string::npos; p = text.find("\nv ", p + 1))
        ++vlines;
    CHECK(vlines == 4 + 9);                                /* 6 - 2 own + 3x3 grid */
    CHECK(text.find("vt 1 1") != std::string::npos);        /* still used by B */
    CHECK(text.find("usemtl red") != std::string::npos);    /* A's single material */
    CHECK(text.find("usemtl blue") != std::string::npos);
    CHECK(text.find("mtllib test.mtl") != std::string::npos);
    CHECK(text.find("\nl ") != std::string::npos);
    CHECK(objscene::list_objects(out)[0].faces == 4);

    /* Replace B, shared with both neighbours: A and C unchanged */
    r.name = "B";
    const std::string outB = temp_path("synthetic_b.obj");
    CHECK(error_of([&] { objscene::splice_obj(in, outB, { r }); }) == "");
    CHECK(same_mesh(in, outB, "A") && same_mesh(in, outB, "C"));

    /* Several materials: the most used one is kept; a replaced object never
       inherits another object's material, the next object keeps the one it
       inherited */
    auto material_before = [](const std::string &text, size_t pos) {
        const size_t m = text.rfind("usemtl ", pos);
        return m == std::string::npos ? std::string()
                                      : text.substr(m + 7, text.find_first_of("\r\n", m) - m - 7);
    };
    write_file(in, std::string(
        "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n"
        "o A\nusemtl red\nf 1 2 3\nusemtl blue\nf 1 3 4\nf 1 2 4\n"
        "o D\nusemtl green\nf 2 3 4\n"
        "o E\nf 1 2 3\n"));
    r.name = "A";
    CHECK(error_of([&] { objscene::splice_obj(in, out, { r }); }) == "");
    std::string t2 = read_text(out);
    CHECK(material_before(t2, t2.find("\nf ", t2.find("o A"))) == "blue");
    CHECK(t2.find("usemtl red") == std::string::npos);
    CHECK(material_before(t2, t2.find("\nf ", t2.find("o E"))) == "green");
    r.name = "E";   /* E only inherits "green": it keeps it */
    CHECK(error_of([&] { objscene::splice_obj(in, out, { r }); }) == "");
    t2 = read_text(out);
    CHECK(material_before(t2, t2.find("\nf ", t2.find("o E"))) == "green");
    r.name = "D";   /* D replaced: E still inherits green */
    CHECK(error_of([&] { objscene::splice_obj(in, out, { r }); }) == "");
    t2 = read_text(out);
    CHECK(material_before(t2, t2.find("\nf ", t2.find("o E"))) == "green");

    /* In place */
    write_file(in, std::string(SYNTHETIC));
    r.name = "A";
    CHECK(error_of([&] { objscene::splice_obj(in, in, { r }); }) == "");
    CHECK(!file_exists(in + ".tmp") && objscene::list_objects(in)[0].faces == 4);

    /* Refused, nothing written */
    const std::string bad = temp_path("synthetic_bad.obj");
    r.name = "Nope";
    CHECK(contains(error_of([&] { objscene::splice_obj(in, bad, { r }); }), "no object named"));
    write_file(temp_path("lines_only.obj"), std::string("v 0 0 0\nv 1 0 0\no L\nl 1 2\n"));
    r.name = "L";
    CHECK(contains(error_of([&] { objscene::splice_obj(temp_path("lines_only.obj"), bad, { r }); }),
                   "has no polygons"));
    CHECK(!file_exists(bad) && !file_exists(bad + ".tmp"));
    write_file(temp_path("bad_index.obj"), std::string("v 0 0 0\no X\nf 1 2 3\n"));
    CHECK(contains(error_of([&] { objscene::list_objects(temp_path("bad_index.obj")); }), "out of range"));
}

static void test_scene_once() {
    std::cout << "objscene: one parse for list, load and splice; CRLF, continued lines" << std::endl;
    const std::string in = temp_path("crlf.obj");
    write_file(in, std::string("# crlf\r\nv 0 0 0\r\nv 1 0 0\r\nv 1 1 \\\r\n 0\r\nv 0 1 0\r\n"
                               "o Q\r\nusemtl m\r\nf 1 2 \\\r\n3 4\r\no R\r\nf 2 3 4\r\n"));
    objscene::Scene scene(in);
    std::vector<objscene::ObjectInfo> objects = scene.objects();
    CHECK(objects.size() == 2 && objects[0].name == "Q" && objects[0].faces == 1 && objects[0].vertices == 4 &&
          objects[1].name == "R" && objects[1].vertices == 3);
    MatrixXu F;
    MatrixXf V;
    scene.load("Q", F, V);
    CHECK(F.cols() == 2 && V.cols() == 4 && V.col(2) == Vector3f(1, 1, 0));

    objscene::Replacement r;
    r.name = "Q";
    make_grid(V, r.F, r.V, 2);
    /* Fetched on demand, as the batch mode does with its spool file */
    objscene::Replacement lazy;
    lazy.name = "Q";
    lazy.fetch = [&](MatrixXu &Fo, MatrixXf &Vo) { Fo = r.F; Vo = r.V; };
    const std::string out = temp_path("crlf_out.obj"), outLazy = temp_path("crlf_lazy.obj");
    CHECK(error_of([&] { scene.splice(out, { r }); }) == "");
    CHECK(error_of([&] { scene.splice(outLazy, { lazy }); }) == "");
    const std::string text = read_text(out);
    CHECK(text == read_text(outLazy));
    size_t lf = 0, crlf = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n') {
            ++lf;
            crlf += i > 0 && text[i - 1] == '\r';
        }
    CHECK(lf > 0 && lf == crlf);
    CHECK(text.find("v 1 1   0") != std::string::npos);   /* continued line, joined */
    CHECK(same_mesh(in, out, "R"));
    CHECK(objscene::list_objects(out)[0].faces == 4);

    /* The same parsed scene can replace its own file */
    CHECK(error_of([&] { scene.splice(in, { r }); }) == "");
    CHECK(!file_exists(in + ".tmp") && objscene::list_objects(in)[0].faces == 4);
}

void test_objscene() {
    test_blender_scene();
    test_synthetic();
    test_scene_once();
}
