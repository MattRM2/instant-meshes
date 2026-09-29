/*
    test_abc.cpp -- Unit tests for the Alembic mesh reader: every reference
    .abc must load exactly like its .obj twin, plus object selection,
    unsupported content and fuzzing.
*/

#include "test_common.h"
#include "abc.h"
#include "meshio.h"
#include "ogawa.h"
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

/* The hashes stored by the Alembic library (Blender's exporter) are the
   oracle for our MurmurHash3 / SpookyHash transcription and hash chain */
static void test_hashes_of_reference_files() {
    std::cout << "abc: recomputed hashes match the Alembic library" << std::endl;
    const char *files[] = { "cube_quads", "ngon_cylinder", "suzanne_open", "hierarchy",
                            "scene_ab", "animated", "instances", "subd" };
    for (const char *name : files) {
        uint64_t objects = 0;
        const std::string msg = error_of([&] {
            objects = abc::verify_hashes(data_path(std::string(name) + ".abc"));
        });
        if (msg != "")
            std::cerr << "  " << name << ": " << msg << std::endl;
        CHECK(msg == "" && objects >= 3);
    }

    /* A single altered value is detected */
    std::vector<uint8_t> bytes = read_file(data_path("cube_quads.abc"));
    abc::Archive ar(data_path("cube_quads.abc"));
    abc::Object mesh;
    abc::Property geom, P;
    CHECK(ar.resolve("/Cube/Cube", mesh) && ar.find(ar.properties(mesh), ".geom", geom) &&
          ar.find(geom, "P", P));
    const uint64_t entry = ar.reader().group(P.group)[0];
    bytes[(size_t) ogawa::entry_pos(entry) + 8 + 16 + 5] ^= 0x10;   /* a byte of P */
    write_file(temp_path("altered.abc"), bytes);
    CHECK(contains(error_of([&] { abc::verify_hashes(temp_path("altered.abc")); }),
                   "sample key mismatch"));
}

/* write_abc -> load_abc gives back exactly the polygons of the extracted
   mesh, including the polygons reassembled from edge quads (F(2) == F(3)) */
static void test_write_roundtrip() {
    std::cout << "abc: writer round trip" << std::endl;
    /* Vertex 9 is used by no polygon (like the centers of irregular faces
       left by quad-dominant extraction): the writer drops it */
    MatrixXf V(3, 10);
    V << 0, 1, 1, 0,   3, 4.5f, 4, 2, 1.5f, 7,
         0, 0, 1, 1,   0, 1,    2.5f, 2.5f, 1, 7,
         0, 0, 0, 0.2f, 0, 0.1f, 0, 0.3f, 0, 7;
    const uint32_t id = 1000;
    MatrixXu F(4, 6);
    F << 0, 4, 5, 6, 7, 8,
         1, 5, 6, 7, 8, 4,
         2, id, id, id, id, id,
         3, id, id, id, id, id;

    std::vector<uint32_t> sizes, indices, faceIds;
    CHECK(extracted_polygons(F, sizes, indices, faceIds) == 1);
    CHECK(sizes == std::vector<uint32_t>({ 4, 5 }));
    std::vector<Vector3f> positions;
    for (int i = 0; i < V.cols(); ++i)
        positions.push_back(V.col(i));
    MatrixXu Fexp, Fgot;
    MatrixXf Vexp, Vgot;
    build_mesh(positions, sizes, indices, Fexp, Vexp, "expected");

    const std::string path = temp_path("roundtrip.abc");
    CHECK(error_of([&] { abc::write_abc(path, F, V); }) == "");
    CHECK(error_of([&] { abc::load_abc(path, Fgot, Vgot); }) == "");
    CHECK(Fgot == Fexp && Vgot == Vexp);
    CHECK(!file_exists(path + ".tmp"));

    /* Written like the library would: every hash verifies */
    uint64_t objects = 0;
    CHECK(error_of([&] { objects = abc::verify_hashes(path); }) == "" && objects == 3);

    /* Named after the file, polygon counts as written */
    std::vector<abc::MeshSummary> meshes = abc::list_meshes(path);
    CHECK(meshes.size() == 1 && meshes[0].path == "/roundtrip/roundtrip" &&
          meshes[0].vertices == 9 && meshes[0].faces == 2);

    /* Refused: empty mesh, invalid positions; the target is left untouched */
    write_file(temp_path("keep.abc"), std::string("KEEP"));
    CHECK(contains(error_of([&] { abc::write_abc(temp_path("keep.abc"), MatrixXu(4, 0), V); }), "empty"));
    MatrixXf Vnan = V;
    Vnan(1, 3) = NAN;
    CHECK(contains(error_of([&] { abc::write_abc(temp_path("keep.abc"), F, Vnan); }), "invalid vertex"));
    CHECK(read_file(temp_path("keep.abc")).size() == 4 && !file_exists(temp_path("keep.abc.tmp")));
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
    test_hashes_of_reference_files();
    test_write_roundtrip();
    test_listing_and_selection();
    test_unsupported();
    test_fuzz_abc(fuzz_scale);
}
