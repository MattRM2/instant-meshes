/*
    test_abc.cpp -- Unit tests for the Alembic mesh reader: every reference
    .abc must load exactly like its .obj twin, plus object selection,
    unsupported content and fuzzing.
*/

#include "test_common.h"
#include "abc.h"
#include "meshio.h"
#include <pcg32.h>
#include <chrono>

/* Cases exported both as OBJ and ABC from the same Blender scene */
static const char *TWIN_CASES[] = {
    "cube_quads", "ngon_cylinder", "suzanne_open", "hierarchy",
    "scene_ab", "animated", "instances"
};

static void test_obj_twins() {
    std::cout << "abc: same triangles and vertices as the OBJ twins" << std::endl;
    for (const char *name : TWIN_CASES) {
        MatrixXu Fo, Fa;
        MatrixXf Vo, Va;
        const std::string msg = error_of([&] {
            load_obj(data_path(std::string(name) + ".obj"), Fo, Vo);
            abc::load_abc(data_path(std::string(name) + ".abc"), Fa, Va);
        });
        if (msg != "")
            std::cerr << "  " << name << ": " << msg << std::endl;
        CHECK(msg == "");

        /* Same topology, bit for bit; positions within OBJ text precision */
        const bool sameF = Fo.rows() == Fa.rows() && Fo.cols() == Fa.cols() && Fo == Fa;
        const bool sameVSize = Vo.cols() == Va.cols();
        const Float dist = sameVSize && Vo.size() > 0 ? (Vo - Va).cwiseAbs().maxCoeff() : -1;
        if (!sameF || !sameVSize || !(dist >= 0 && dist < 1e-5f))
            std::cerr << "  " << name << ": F " << Fo.cols() << " vs " << Fa.cols()
                      << (sameF ? " (same)" : " (DIFFERENT)") << ", V " << Vo.cols()
                      << " vs " << Va.cols() << ", max distance " << dist << std::endl;
        CHECK(sameF);
        CHECK(sameVSize && dist >= 0 && dist < 1e-5f);
    }
}

/* Writes the ABC geometry as a full-precision OBJ ("exact_<case>.obj" in the
   temp directory). load_obj() must give back exactly the same F and V, so
   remeshing that OBJ and the ABC gives byte-identical results: any
   difference with Blender's OBJ export only comes from its 6-decimal text. */
static void test_exact_obj() {
    std::cout << "abc: full-precision OBJ round trip" << std::endl;
    for (const char *name : TWIN_CASES) {
        MatrixXu F, F2;
        MatrixXf V, V2;
        const std::string path = temp_path(std::string("exact_") + name + ".obj");
        const std::string msg = error_of([&] {
            abc::load_abc(data_path(std::string(name) + ".abc"), F, V);
            FILE *f = fopen(path.c_str(), "w");
            if (!f)
                throw std::runtime_error("cannot write " + path);
            for (int i = 0; i < V.cols(); ++i)
                fprintf(f, "v %.9g %.9g %.9g\n", V(0, i), V(1, i), V(2, i));
            for (int i = 0; i < F.cols(); ++i)
                fprintf(f, "f %u %u %u\n", F(0, i) + 1, F(1, i) + 1, F(2, i) + 1);
            fclose(f);
            load_obj(path, F2, V2);
        });
        CHECK(msg == "");
        CHECK(F == F2 && V == V2);
    }
}

static void test_listing_and_selection() {
    std::cout << "abc: mesh listing and object selection" << std::endl;

    std::vector<abc::MeshSummary> meshes;
    CHECK(error_of([&] { meshes = abc::list_meshes(data_path("scene_ab.abc")); }) == "");
    CHECK(meshes.size() == 2);
    std::string pathA, pathB;
    for (const abc::MeshSummary &m : meshes) {
        std::cout << "  scene_ab: " << m.path << " (V=" << m.vertices << ", faces=" << m.faces << ")" << std::endl;
        if (contains(m.path, "MeshA")) {
            pathA = m.path;
            CHECK(m.vertices == 7958 && m.faces == 7872);
        } else {
            pathB = m.path;
            CHECK(m.vertices == 576 && m.faces == 576);
        }
    }
    CHECK(pathA != "" && pathB != "");

    /* The parent Xform path selects the mesh below it */
    const std::string xformA = pathA.substr(0, pathA.rfind('/'));
    MatrixXu F;
    MatrixXf V;
    CHECK(error_of([&] { abc::load_abc(data_path("scene_ab.abc"), F, V, xformA); }) == "");
    CHECK(V.cols() == 7958 && F.cols() == 2 * 7872);
    CHECK(error_of([&] { abc::load_abc(data_path("scene_ab.abc"), F, V, pathB); }) == "");
    CHECK(V.cols() == 576 && F.cols() == 2 * 576);

    /* A common parent selects both */
    const std::string parent = xformA.substr(0, xformA.rfind('/'));
    CHECK(error_of([&] { abc::load_abc(data_path("scene_ab.abc"), F, V, parent); }) == "");
    CHECK(V.cols() == 7958 + 576);

    /* Missing object, and a name prefix that is not a whole path component */
    CHECK(contains(error_of([&] { abc::load_abc(data_path("scene_ab.abc"), F, V, "/Nope"); }),
                   "no polygon mesh at"));
    CHECK(contains(error_of([&] {
        abc::load_abc(data_path("scene_ab.abc"), F, V, xformA.substr(0, xformA.size() - 1));
    }), "no polygon mesh at"));

    /* Instances are listed under their own path */
    CHECK(error_of([&] { meshes = abc::list_meshes(data_path("instances.abc")); }) == "");
    CHECK(meshes.size() == 2 && meshes[0].path != meshes[1].path);
    for (const abc::MeshSummary &m : meshes)
        std::cout << "  instances: " << m.path << " (V=" << m.vertices << ")" << std::endl;
}

static void test_unsupported() {
    std::cout << "abc: unsupported content" << std::endl;
    MatrixXu F;
    MatrixXf V;
    CHECK(contains(error_of([&] { abc::load_abc(data_path("subd.abc"), F, V); }),
                   "only subdivision surfaces"));
    CHECK(contains(error_of([&] { abc::load_abc(data_path("cube_quads.obj"), F, V); }),
                   "not an Alembic"));
    CHECK(contains(error_of([&] { abc::load_abc(temp_path("missing.abc"), F, V); }),
                   "Unable to open"));

    /* Through the generic entry point used by the GUI and batch mode */
    MatrixXf N;
    CHECK(error_of([&] { load_mesh_or_pointcloud(data_path("hierarchy.abc"), F, V, N); }) == "");
    CHECK(F.cols() == 92 && V.cols() == 50);
}

static void test_fuzz_abc(int scale) {
    std::cout << "abc: fuzzing the mesh reader" << std::endl;
    pcg32 rng;
    rng.seed(0xabc, 0x5eed);
    size_t runs = 0, accepted = 0, rejected = 0, unexpected = 0;
    auto t0 = std::chrono::steady_clock::now();

    const char *files[] = { "cube_quads.abc", "ngon_cylinder.abc", "hierarchy.abc",
                            "animated.abc", "instances.abc" };
    for (const char *name : files) {
        const std::vector<uint8_t> original = read_file(data_path(name));
        const std::string path = temp_path(std::string("fuzzabc_") + name);
        auto run = [&](const std::vector<uint8_t> &bytes) {
            write_file(path, bytes);
            ++runs;
            try {
                MatrixXu F;
                MatrixXf V;
                abc::load_abc(path, F, V);
                ++accepted;
            } catch (const std::runtime_error &) {
                ++rejected;
            } catch (...) {
                ++unexpected;
            }
        };
        for (int i = 0; i < 300 * scale; ++i) {
            std::vector<uint8_t> bytes = original;
            const uint32_t flips = 1 + rng.nextUInt(8);
            for (uint32_t k = 0; k < flips; ++k) {
                const uint32_t at = rng.nextUInt((uint32_t) bytes.size());
                switch (rng.nextUInt(3)) {
                    case 0: bytes[at] ^= (uint8_t) (1 << rng.nextUInt(8)); break;
                    case 1: bytes[at] = (uint8_t) rng.nextUInt(256); break;
                    default: bytes[at] = rng.nextUInt(2) ? 0xff : 0x00; break;
                }
            }
            run(bytes);
        }
        for (int i = 0; i < 40 * scale; ++i) {
            const size_t len = rng.nextUInt((uint32_t) original.size());
            run(std::vector<uint8_t>(original.begin(), original.begin() + len));
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  " << runs << " corrupted files: " << accepted << " loaded, " << rejected
              << " rejected, " << unexpected << " unexpected errors (" << (int) (s * 1000)
              << " ms)" << std::endl;
    CHECK(unexpected == 0);
}

void test_abc(int fuzz_scale) {
    test_obj_twins();
    test_exact_obj();
    test_listing_and_selection();
    test_unsupported();
    test_fuzz_abc(fuzz_scale);
}
