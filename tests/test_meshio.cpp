/*
    test_meshio.cpp -- Unit tests for the mesh readers (polygon triangulation,
    OBJ loader error handling, reference dataset).
*/

#include "test_common.h"
#include "meshio.h"
#include <algorithm>
#include <cmath>

/* ------------------------------------------------------------------------- */
/*  triangulate_polygon                                                      */
/* ------------------------------------------------------------------------- */

static Vector3f newell(const std::vector<Vector3f> &p) {
    Vector3f n = Vector3f::Zero();
    for (size_t i = 0; i < p.size(); ++i) {
        const Vector3f &a = p[i], &b = p[(i + 1) % p.size()];
        n += Vector3f((a.y() - b.y()) * (a.z() + b.z()),
                      (a.z() - b.z()) * (a.x() + b.x()),
                      (a.x() - b.x()) * (a.y() + b.y()));
    }
    return n * 0.5f;   /* vector area */
}

/* For a simple planar polygon, a valid triangulation has n-2 triangles, all
   oriented like the polygon, whose areas add up to the polygon area (any
   overlap or triangle outside the polygon breaks the sum). */
static bool valid_triangulation(const std::vector<Vector3f> &p,
                                const std::vector<uint32_t> &tris,
                                bool allow_degenerate = false) {
    const size_t n = p.size();
    if (tris.size() != 3 * (n - 2))
        return false;
    const Vector3f area = newell(p);
    const Vector3f dir = area.normalized();
    Float sum = 0;
    for (size_t t = 0; t < tris.size(); t += 3) {
        for (int k = 0; k < 3; ++k)
            if (tris[t + k] >= n)
                return false;
        const Vector3f tn = 0.5f * (p[tris[t + 1]] - p[tris[t]]).cross(p[tris[t + 2]] - p[tris[t]]);
        const Float a = tn.dot(dir);
        if (a < -1e-6f || (!allow_degenerate && a < 1e-7f))
            return false;
        sum += a;
    }
    return std::abs(sum - area.norm()) < 1e-4f * std::max((Float) 1, area.norm());
}

/* Places 2D outline points on an arbitrary tilted plane in 3D */
static std::vector<Vector3f> on_plane(const std::vector<Vector2f> &pts) {
    const Vector3f o(0.3f, -1.2f, 2.0f);
    const Vector3f ex = Vector3f(1, 2, 0.5f).normalized();
    const Vector3f ey = ex.cross(Vector3f(0.2f, -0.4f, 1)).normalized().cross(ex);
    std::vector<Vector3f> out;
    for (const Vector2f &q : pts)
        out.push_back(o + q.x() * ex + q.y() * ey);
    return out;
}

static std::vector<uint32_t> tri(const std::vector<Vector3f> &p) {
    std::vector<uint32_t> t;
    triangulate_polygon(p, t);
    return t;
}

static void test_triangulate() {
    std::cout << "triangulate_polygon" << std::endl;

    /* Triangles and quads: historical split, unchanged */
    std::vector<Vector3f> t3 = on_plane({{0, 0}, {1, 0}, {0, 1}});
    CHECK(tri(t3) == std::vector<uint32_t>({0, 1, 2}));
    std::vector<Vector3f> q4 = on_plane({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    CHECK(tri(q4) == std::vector<uint32_t>({0, 1, 2, 3, 0, 2}));

    /* Convex pentagon */
    CHECK(valid_triangulation(on_plane({{0, 0}, {2, 0}, {3, 1}, {1, 2}, {-1, 1}}),
                              tri(on_plane({{0, 0}, {2, 0}, {3, 1}, {1, 2}, {-1, 1}}))));

    /* Square with edge midpoints: collinear corners, no flat triangle allowed */
    std::vector<Vector3f> sq = on_plane({{0, 0}, {1, 0}, {2, 0}, {2, 1}, {2, 2},
                                         {1, 2}, {0, 2}, {0, 1}});
    CHECK(valid_triangulation(sq, tri(sq)));

    /* Concave L shape (a fan from corner 0 would leave the polygon) */
    std::vector<Vector3f> L = on_plane({{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}});
    CHECK(valid_triangulation(L, tri(L)));
    std::vector<Vector3f> L2 = on_plane({{1, 1}, {1, 2}, {0, 2}, {0, 0}, {2, 0}, {2, 1}});
    CHECK(valid_triangulation(L2, tri(L2)));

    /* Concave 10-point star, both windings */
    std::vector<Vector2f> star;
    for (int i = 0; i < 10; ++i) {
        const Float r = (i % 2) ? 0.4f : 1.0f, a = (Float) (2 * M_PI * i / 10);
        star.push_back(Vector2f(r * std::cos(a), r * std::sin(a)));
    }
    std::vector<Vector3f> s = on_plane(star);
    CHECK(valid_triangulation(s, tri(s)));
    std::reverse(star.begin(), star.end());
    s = on_plane(star);
    CHECK(valid_triangulation(s, tri(s)));

    /* Comb: many reflex corners */
    std::vector<Vector2f> comb = {{0, 0}, {9, 0}, {9, 3}};
    for (int i = 8; i >= 1; --i)
        comb.push_back(Vector2f((Float) i, (i % 2) ? 1.0f : 3.0f));
    comb.push_back(Vector2f(0, 3));
    s = on_plane(comb);
    CHECK(valid_triangulation(s, tri(s)));

    /* Large convex n-gon (CAD-style cylinder cap). Its legitimate slivers
       (~1e-8) are below float noise, so only exact area coverage is checked. */
    std::vector<Vector2f> circle;
    for (int i = 0; i < 2000; ++i) {
        const Float a = (Float) (2 * M_PI * i / 2000);
        circle.push_back(Vector2f(std::cos(a), std::sin(a)));
    }
    s = on_plane(circle);
    CHECK(valid_triangulation(s, tri(s), true));

    /* Garbage in: must not crash, must return n-2 in-range triangles */
    auto sane = [](const std::vector<Vector3f> &p) {
        std::vector<uint32_t> t = tri(p);
        if (t.size() != 3 * (p.size() - 2))
            return false;
        for (uint32_t i : t)
            if (i >= p.size())
                return false;
        return true;
    };
    CHECK(sane(on_plane({{0, 0}, {2, 2}, {2, 0}, {0, 2}, {1, 3}})));        /* self-intersecting */
    CHECK(sane(std::vector<Vector3f>(7, Vector3f(1, 2, 3))));               /* all points equal  */
    CHECK(sane(on_plane({{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}})));         /* all collinear     */
    std::vector<Vector3f> nanp = on_plane({{0, 0}, {1, 0}, {1, 1}, {0.5f, 2}, {0, 1}});
    nanp[2] = Vector3f(NAN, 0, 0);
    CHECK(sane(nanp));                                                       /* NaN corner        */

    CHECK(error_of([] { std::vector<uint32_t> t; triangulate_polygon(std::vector<Vector3f>(2), t); }) != "");
}

/* ------------------------------------------------------------------------- */
/*  load_obj                                                                 */
/* ------------------------------------------------------------------------- */

static void test_load_obj() {
    std::cout << "load_obj" << std::endl;
    MatrixXu F;
    MatrixXf V;

    /* Reference dataset: every polygon triangulated (n-gons used to be cut
       down to their first 4 corners) */
    load_obj(data_path("ngon_cylinder.obj"), F, V);
    CHECK(F.cols() == 50 && V.cols() == 27);
    load_obj(data_path("scene_ab.obj"), F, V);
    CHECK(F.cols() == 16896 && V.cols() == 8534);

    /* Concave n-gon in a file */
    write_file(temp_path("concave.obj"),
               "v 0 0 0\nv 2 0 0\nv 2 1 0\nv 1 1 0\nv 1 2 0\nv 0 2 0\nf 1 2 3 4 5 6\n");
    load_obj(temp_path("concave.obj"), F, V);
    CHECK(F.cols() == 4 && V.cols() == 6);

    /* v/vt/vn corner syntax */
    write_file(temp_path("slashes.obj"),
               "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nvt 0 0\nvn 0 0 1\n"
               "f 1/1/1 2/1/1 3/1/1 4/1/1\nf 1//1 3//1 4//1\n");
    load_obj(temp_path("slashes.obj"), F, V);
    CHECK(F.cols() == 3 && V.cols() == 4);

    /* Broken files: clear error, never a crash */
    struct { const char *name, *content, *expect; } bad[] = {
        {"two_corners.obj",  "v 0 0 0\nv 1 0 0\nf 1 2\n",             "fewer than 3"},
        {"index_zero.obj",   "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 0 1 2\n",  "out of range"},
        {"index_high.obj",   "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\n",  "out of range"},
        {"index_neg.obj",    "v 0 0 0\nv 1 0 0\nv 0 1 0\nf -4 -2 -1\n", "out of range"},
        {"garbage.obj",      "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 x\n",  "parse"},
        {"too_many_slash.obj","v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1/1/1/1 2 3\n", "Invalid vertex data"},
    };
    for (const auto &b : bad) {
        write_file(temp_path(b.name), b.content);
        const std::string msg = error_of([&] { load_obj(temp_path(b.name), F, V); });
        const bool ok = msg.find(b.expect) != std::string::npos;
        if (!ok)
            std::cerr << "  " << b.name << ": got \"" << msg << "\"" << std::endl;
        CHECK(ok);
    }
    CHECK(error_of([&] { load_obj(temp_path("does_not_exist.obj"), F, V); }).find("Unable to open") != std::string::npos);

    /* Empty file: no faces, no vertices, no crash */
    write_file(temp_path("empty.obj"), "");
    CHECK(error_of([&] { load_obj(temp_path("empty.obj"), F, V); }) == "" && F.cols() == 0);
}

void test_meshio() {
    test_triangulate();
    test_load_obj();
}
