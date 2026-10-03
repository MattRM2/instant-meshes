/*
    test_usd.cpp -- Unit tests for the USD layer reader (usd.h): the same
    scene in .usda / .usdc / .usdz (tests/usd_reference.py, Pixar's USD
    library), reading and fuzzing.
*/

#include "test_common.h"
#include "usd.h"
#include "usdc.h"
#include "usdscene.h"
#include "meshio.h"
#include <pcg32.h>
#include <chrono>
#include <functional>

using namespace usd;

/* Every value of a layer, decoded (exercises the lazy .usdc decoding) */
static size_t touch_all(const Layer &layer, const Prim &p) {
    size_t n = 1;
    for (const Property &q : p.properties) {
        layer.value(q);
        layer.samples(q);
    }
    for (const auto &c : p.children)
        n += touch_all(layer, *c);
    for (const auto &set : p.variants)
        for (const auto &v : set.second)
            n += touch_all(layer, *v.second);
    return n;
}

static void test_read() {
    std::cout << "usd: reading .usda / .usdc / .usdz" << std::endl;
    for (const char *name : { "usd_scene.usda", "usd_scene.usdc", "usd_scene.usdz" }) {
        std::unique_ptr<Layer> layer;
        CHECK(error_of([&] { layer.reset(new Layer(data_path(name))); }) == "");
        if (!layer)
            continue;
        CHECK(layer->meta["upAxis"].str() == "Y" && std::abs(layer->meta["metersPerUnit"].num() - 0.01) < 1e-9 &&
              layer->meta["defaultPrim"].str() == "World");
        const Prim *a = layer->prim("/World/geo/MeshA");
        CHECK(a && a->type == "Mesh" && a->specifier == Specifier::Def);
        if (a) {
            const Property *points = a->property("points");
            CHECK(points && points->type == "point3f[]" && layer->value(*points).numbers.size() == 48);
            const Property *st = a->property("primvars:st");
            CHECK(st && layer->value(*st).numbers.size() == 96 && st->meta.count("interpolation") &&
                  st->meta.at("interpolation").str() == "faceVarying");
            const Property *order = a->property("xformOpOrder");
            CHECK(order && order->uniform && layer->value(*order).strings.size() == 3 &&
                  layer->value(*order).strings[1] == "xformOp:rotateXYZ");
            const Property *binding = a->property("material:binding");
            CHECK(binding && binding->relationship && binding->targets.size() == 1 &&
                  binding->targets[0] == "/World/mtl/blue");
        }
        /* quaternion in text order (w, x, y, z) */
        const Prim *g = layer->prim("/World/geo/Group");
        const Property *orient = g ? g->property("xformOp:orient") : nullptr;
        CHECK(orient && std::abs(layer->value(*orient).numbers[0] - 0.9238795) < 1e-6 &&
              std::abs(layer->value(*orient).numbers[2] - 0.3826834) < 1e-6);
        /* the big grid: compressed arrays in .usdc */
        const Prim *c = layer->prim("/World/geo/MeshC");
        const Property *idx = c ? c->property("faceVertexIndices") : nullptr;
        CHECK(idx && layer->value(*idx).numbers.size() == 4800 && layer->value(*idx).numbers[4799] == 1229);
        /* time samples */
        const Prim *spin = layer->prim("/World/geo/Spin");
        const Property *rot = spin ? spin->property("xformOp:rotateY") : nullptr;
        CHECK(rot && rot->hasTimeSamples && layer->samples(*rot).samples.size() == 2 &&
              layer->samples(*rot).samples[1].first == 10 && layer->samples(*rot).samples[1].second.num() == 90);
        /* class prototype, instance, variants */
        const Prim *proto = layer->prim("/Proto");
        CHECK(proto && proto->specifier == Specifier::Class);
        const Prim *var = layer->prim("/World/geo/Var");
        CHECK(var && var->variants.count("lod") && var->variants.at("lod").size() == 2);
        CHECK(touch_all(*layer, layer->root) > 15);
    }
    CHECK(contains(error_of([&] { Layer l(data_path("scene_ab.abc")); }), "not a USD layer"));
    CHECK(contains(error_of([&] { Layer l(temp_path("missing.usd")); }), "Unable to open"));
}

static void test_fuzz(int scale) {
    std::cout << "usd: fuzzing the readers" << std::endl;
    pcg32 rng;
    rng.seed(0x05d, 0x5eed);
    size_t runs = 0, accepted = 0, rejected = 0, unexpected = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const char *name : { "usd_scene.usdc", "usd_scene.usda", "usd_scene.usdz" }) {
        const std::vector<uint8_t> original = read_file(data_path(name));
        const std::string path = temp_path(std::string("fuzz_") + name);
        auto run = [&](const std::vector<uint8_t> &bytes) {
            write_file(path, bytes);
            ++runs;
            try {
                Layer layer(path);
                touch_all(layer, layer.root);
                ++accepted;
            } catch (const std::runtime_error &) {
                ++rejected;
            } catch (...) {
                ++unexpected;
            }
        };
        for (int i = 0; i < 400 * scale; ++i) {
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
        for (int i = 0; i < 60 * scale; ++i) {
            const size_t len = rng.nextUInt((uint32_t) original.size());
            run(std::vector<uint8_t>(original.begin(), original.begin() + len));
        }
    }
    /* deep nesting in text */
    write_file(temp_path("deep.usda"), "#usda 1.0\ndef \"A\" { float[] a = " + std::string(100000, '[') + " }\n");
    CHECK(contains(error_of([&] { Layer l(temp_path("deep.usda")); }), "nested too deeply"));
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  " << runs << " corrupted files: " << accepted << " loaded, " << rejected << " rejected, "
              << unexpected << " unexpected errors (" << (int) (s * 1000) << " ms)" << std::endl;
    CHECK(unexpected == 0);
}

/* The meshes in world space, against the twin that Pixar's USD wrote
   (tests/usd_reference.py: composed stage, first frame, left-handed meshes
   reversed): every transform op, the quaternion, the matrix, the animated
   parent, the orientation */
static void test_world() {
    std::cout << "usd: meshes in world space, as Pixar's USD places them" << std::endl;
    MatrixXu Ft;
    MatrixXf Vt;
    load_obj(data_path("usd_scene_world.obj"), Ft, Vt);
    for (const char *name : { "usd_scene.usda", "usd_scene.usdc", "usd_scene.usdz" }) {
        MatrixXu F;
        MatrixXf V, N;
        CHECK(error_of([&] { load_mesh_or_pointcloud(data_path(name), F, V, N); }) == "");
        CHECK(F == Ft && V.cols() == Vt.cols());
        if (V.cols() == Vt.cols())
            CHECK((V - Vt).cwiseAbs().maxCoeff() < 1e-5f);
    }
    /* one mesh, per-object loading: the same triangles */
    Layer layer(data_path("usd_scene.usdc"));
    std::vector<abc::MeshSummary> meshes = list_meshes(layer);
    CHECK(meshes.size() == 5 && meshes[0].path == "/World/geo/MeshA" && meshes[0].faces == 10 &&
          meshes[0].vertices == 16 && meshes[2].faces == 1200 && meshes[3].animated && !meshes[1].animated);
    MatrixXu F;
    MatrixXf V;
    std::vector<UVSet> uvs;
    load_mesh(layer, "/World/geo/Group/MeshB", F, V, nullptr, &uvs);
    CHECK(F.cols() == 6 && uvs.size() == 1 && uvs[0].name == "uv" && uvs[0].corners.size() == 18);
    load_mesh(layer, "/World/geo/MeshA", F, V, nullptr, &uvs);
    CHECK(uvs.size() == 1 && uvs[0].name == "st" && uvs[0].corners.size() == 3 * (size_t) F.cols());
    CHECK(contains(error_of([&] { load_mesh(layer, "/Proto/Box", F, V); }), "no polygon mesh"));
}

void test_usd(int fuzz_scale) {
    test_read();
    test_world();
    test_fuzz(fuzz_scale);
}
