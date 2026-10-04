/*
    test_project.cpp -- Unit tests for the projects (.imd) and the LZ4
    compressor they use.
*/

#include "test_common.h"
#include "project.h"
#include "usdc.h"
#include "usdcwrite.h"
#include <pcg32.h>

static void test_lz4() {
    std::cout << "project: LZ4 compression" << std::endl;
    pcg32 rng;
    for (size_t size : { 0, 1, 12, 13, 64, 1000, 70000, 300000 }) {
        for (int kind = 0; kind < 3; ++kind) {
            std::vector<uint8_t> data(size);
            for (size_t i = 0; i < size; ++i)
                data[i] = kind == 0 ? (uint8_t) rng.nextUInt(256)                  /* noise */
                        : kind == 1 ? (uint8_t) (i % 7)                             /* short period */
                        : (uint8_t) ((i / 1000) * 31 + (rng.nextUInt(16) == 0));    /* runs */
            const std::vector<uint8_t> z = usd::lz4_compress(data.data(), data.size());
            std::vector<uint8_t> back(size + 1);
            const size_t n = usd::lz4_block(z.data(), z.size(), back.data(), back.size());
            back.resize(n);
            CHECK(back == data);
            if (kind == 1 && size >= 1000)
                CHECK(z.size() < size / 10);
        }
    }
}

static void test_save_load() {
    std::cout << "project: save, load, results, stale meshes" << std::endl;
    const std::string scene = temp_path("project_scene.obj");
    write_file(scene, read_file(data_path("scene_ab.obj")));
    const std::string imd = temp_path("project.imd");

    std::unique_ptr<Project> p = Project::create(scene);
    CHECK(p->objects.size() == 2 && p->objects[0].mesh.path == "/MeshA");
    p->options.others = parse_face_target("90%");
    p->options.params.deterministic = true;
    p->options.params.uv = RemeshParams::UVUnwrap;
    p->apply_rules({ parse_mesh_rule("MeshB=150%") });
    CHECK(p->objects[1].rule == "MeshB=150%" && p->target_of(p->objects[0]).text == "90%");
    CHECK(contains(error_of([&] { p->apply_rules({ parse_mesh_rule("Nope=50%") }); }), "no polygon mesh matches"));
    p->process(p->objects[1]);
    CHECK(p->objects[1].state == ObjectState::Done && p->objects[1].resultFaces > 576);
    p->objects[0].checked = true;
    p->objects[0].work = { 1, 2, 3 };
    p->ui["outliner.sort"] = "faces desc";
    MatrixXu F1;
    MatrixXf V1;
    std::vector<CornerUVs> uv1;
    p->objects[1].result(F1, V1, uv1);
    CHECK(error_of([&] { p->save(imd); }) == "");
    /* the results now come from the project file */
    MatrixXu F2;
    MatrixXf V2;
    std::vector<CornerUVs> uv2;
    p->objects[1].result(F2, V2, uv2);
    CHECK(F2 == F1 && V2 == V1 && uv2.size() == 1 && uv2[0].corners == uv1[0].corners);
    p.reset();

    std::unique_ptr<Project> q;
    CHECK(error_of([&] { q = Project::load(imd); }) == "");
    if (!q)
        return;
    CHECK(q->objects.size() == 2 && q->options.others.text == "90%" && q->options.params.deterministic &&
          q->options.params.uv == RemeshParams::UVUnwrap);
    CHECK(q->objects[1].state == ObjectState::Done && q->objects[1].rule == "MeshB=150%" &&
          q->objects[0].checked && q->objects[0].work == std::vector<uint8_t>({ 1, 2, 3 }) &&
          q->ui["outliner.sort"] == "faces desc");
    MatrixXu F3;
    MatrixXf V3;
    std::vector<CornerUVs> uv3;
    q->objects[1].result(F3, V3, uv3);
    CHECK(F3 == F1 && V3 == V1 && uv3.size() == 1 && uv3[0].name == "UVMap");

    /* the scene moved next to the project: found again */
    q.reset();
    const std::string moved = temp_path("project_moved_scene.obj");
    write_file(moved, read_file(scene));
    CHECK(error_of([&] { q = Project::load(imd, moved); }) == "" && q && q->source == moved);

    /* the scene changed: MeshB has other geometry, its result is stale */
    q.reset();
    std::string text;
    {
        const std::vector<uint8_t> b = read_file(scene);
        text.assign(b.begin(), b.end());
    }
    const size_t b = text.find("o MeshB");
    text = text.substr(0, b) + "o MeshB\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf -3 -2 -1\n";
    write_file(scene, text);
    CHECK(error_of([&] { q = Project::load(imd); }) == "");
    CHECK(q && q->objects[1].state == ObjectState::Stale && q->objects[0].state == ObjectState::Pending);

    /* damaged files */
    std::vector<uint8_t> bytes = read_file(imd);
    bytes[bytes.size() / 2] ^= 0x5a;
    write_file(temp_path("project_bad.imd"), bytes);
    CHECK(error_of([&] {
        std::unique_ptr<Project> r = Project::load(temp_path("project_bad.imd"));
        for (ProjectObject &o : r->objects)
            if (o.result) {
                MatrixXu F;
                MatrixXf V;
                std::vector<CornerUVs> uv;
                o.result(F, V, uv);
            }
    }) != "");
    write_file(temp_path("project_not.imd"), std::string("hello"));
    CHECK(contains(error_of([&] { Project::load(temp_path("project_not.imd")); }), "not an Instant Meshes project"));
}

void test_project() {
    test_lz4();
    test_save_load();
}
