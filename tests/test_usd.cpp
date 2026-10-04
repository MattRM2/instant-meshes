/*
    test_usd.cpp -- Unit tests for the USD layer reader (usd.h): the same
    scene in .usda / .usdc / .usdz (tests/usd_reference.py, Pixar's USD
    library), reading and fuzzing.
*/

#include "test_common.h"
#include "usd.h"
#include "usdc.h"
#include "usdscene.h"
#include "usdstage.h"
#include "usdcwrite.h"
#include "usdedit.h"
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

/* Where --proxy puts the proxies: geo/render mirrored to geo/proxy (even
   below an animated transform), else <name>_proxy next to the mesh */
static void test_proxy_paths() {
    std::cout << "usd: proxy placement" << std::endl;
    Layer asset(data_path("usd_asset.usdc"));
    std::vector<abc::MeshSummary> meshes = list_meshes(asset);
    CHECK(meshes.size() == 4 && meshes[3].path == "/Asset/geo/guide/Helper" && meshes[3].purpose == "guide" &&
          meshes[0].purpose.empty());
    std::vector<std::string> where = proxy_paths(asset, { "/Asset/geo/render/Body", "/Asset/geo/render/Spinner/Wheel" });
    CHECK(where.size() == 2 && where[0] == "/Asset/geo/proxy/Body" && where[1] == "/Asset/geo/proxy/Spinner/Wheel");
    CHECK(contains(error_of([&] { proxy_paths(asset, { "/Asset/geo/guide/Helper" }); }), "purpose \"guide\""));
    Layer scene(data_path("usd_scene.usdc"));
    where = proxy_paths(scene, { "/World/geo/MeshA", "/World/geo/Spin/MeshE" });
    CHECK(where.size() == 2 && where[0] == "/World/geo/MeshA_proxy" && where[1] == "/World/geo/Spin/MeshE_proxy");
}

/* A composed stage (tests/data/compose: sublayer, references, payload,
   variants, inherits, specializes, instance): the meshes where Pixar's USD
   places them, the material bindings mapped into the stage */
static void test_compose() {
    std::cout << "usd: composed stage, meshes in world space" << std::endl;
    MatrixXu Ft;
    MatrixXf Vt;
    load_obj(data_path("compose/assembly_world.obj"), Ft, Vt);
    for (const char *name : { "compose/assembly.usda", "compose/assembly.usdc" }) {
        MatrixXu F;
        MatrixXf V, N;
        CHECK(error_of([&] { load_mesh_or_pointcloud(data_path(name), F, V, N); }) == "");
        CHECK(F == Ft && V.cols() == Vt.cols());
        if (V.cols() == Vt.cols())
            CHECK((V - Vt).cwiseAbs().maxCoeff() < 1e-5f);
    }
    std::shared_ptr<const Layer> stage = open_stage(data_path("compose/assembly.usdc"));
    const Prim *box = stage->prim("/World/propB/geo/render/Box");
    const Property *binding = box ? box->property("material:binding") : nullptr;
    CHECK(binding && binding->targets.size() == 1 && binding->targets[0] == "/World/propB/mtl/wood");
    std::vector<abc::MeshSummary> meshes = list_meshes(*stage);
    size_t instanced = 0;
    for (const abc::MeshSummary &m : meshes)
        instanced += m.instanced;
    CHECK(meshes.size() == 8 && instanced == 1 && !stage->prim("/World/off/geo"));
    /* a layer without arcs is read as is */
    std::shared_ptr<const Layer> plain = open_stage(data_path("usd_asset.usdc"));
    CHECK(plain->sources.empty() && plain->prim("/Asset/geo/render/Body"));
}

/* The .usdc writer: its LZ4 blocks and integer coding read back by the
   reader; a layer written as .usdc / .usdz reads as its text */
static void test_write() {
    std::cout << "usd: .usdc / .usdz writing" << std::endl;
    pcg32 rng;
    for (size_t size : { 0, 1, 14, 15, 16, 269, 270, 271, 5000 }) {
        std::vector<uint8_t> data(size);
        for (uint8_t &b : data)
            b = (uint8_t) rng.nextUInt(256);
        std::vector<uint8_t> c { 0 };
        const std::vector<uint8_t> block = lz4_literals(data.data(), data.size());
        c.insert(c.end(), block.begin(), block.end());
        CHECK(fast_decompress(c.data(), c.size(), size + 16) == data);
    }
    const std::string text =
        "#usda 1.0\n(\n    defaultPrim = \"W\"\n    metersPerUnit = 0.01\n    upAxis = \"Z\"\n)\n"
        "def Xform \"W\" (\n    kind = \"component\"\n)\n{\n"
        "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:orient\", \"xformOp:rotateY\"]\n"
        "    double3 xformOp:translate = (1.5, -2, 1e10)\n"
        "    quatf xformOp:orient = (0.5, 0.5, -0.5, 0.5)\n"
        "    float xformOp:rotateY.timeSamples = { 1: 0, 24: 90, }\n"
        "    def Mesh \"M\" (\n        prepend apiSchemas = [\"MaterialBindingAPI\"]\n    )\n    {\n"
        "        int[] faceVertexCounts = [3, 4]\n"
        "        int[] faceVertexIndices = [0, 1, 2, 0, 2, 3, 100000]\n"
        "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
        "        texCoord2h[] primvars:st = [(0, 0.5), (1, 0.25)] (\n            interpolation = \"vertex\"\n        )\n"
        "        normal3f[] normals = None\n"
        "        uniform token subdivisionScheme = \"none\"\n"
        "        matrix4d m = ((1, 0, 0, 0), (0, 2, 0, 0), (0, 0, 3, 0), (4, 5, 6, 1))\n"
        "        rel material:binding = </W/mtl.outputs:surface>\n"
        "    }\n}\n";
    std::map<std::string, Value> meta;
    Prim root;
    root.path = "/";
    parse_usda(text, "test", meta, root);
    write_file(temp_path("write_source.usda"), text);
    Layer original(temp_path("write_source.usda"));
    for (const char *ext : { "usdc", "usdz" }) {
        const std::string out = temp_path(std::string("write_test.") + ext);
        CHECK(error_of([&] { write_layer_file(out, text, out); }) == "");
        std::unique_ptr<Layer> back;
        CHECK(error_of([&] { back.reset(new Layer(out)); }) == "");
        if (!back)
            continue;
        CHECK(back->format() == ext && back->meta["upAxis"].str() == "Z" &&
              std::abs(back->meta["metersPerUnit"].num() - 0.01) < 1e-12);
        std::function<void(const Prim &, const Prim &)> same = [&](const Prim &a, const Prim &b) {
            CHECK(a.path == b.path && a.type == b.type && a.specifier == b.specifier &&
                  a.properties.size() == b.properties.size() && a.children.size() == b.children.size());
            for (const Property &p : a.properties) {
                const Property *q = b.property(p.name);
                CHECK(q && q->type == p.type && q->uniform == p.uniform && q->targets == p.targets &&
                      q->hasTimeSamples == p.hasTimeSamples);
                if (!q)
                    continue;
                const Value va = original.value(p), vb = back->value(*q);
                CHECK(va.kind == vb.kind && va.strings == vb.strings && va.numbers.size() == vb.numbers.size());
                for (size_t k = 0; k < va.numbers.size() && k < vb.numbers.size(); ++k)
                    CHECK(std::abs(va.numbers[k] - vb.numbers[k]) <= 1e-3 * std::max(1.0, std::abs(va.numbers[k])));
                if (p.hasTimeSamples)
                    CHECK(original.samples(p).samples.size() == back->samples(*q).samples.size() &&
                          back->samples(*q).samples[1].second.num() == 90);
            }
            for (size_t k = 0; k < a.children.size() && k < b.children.size(); ++k)
                same(*a.children[k], *b.children[k]);
        };
        same(original.root, back->root);
        const Prim *m = back->prim("/W/M");
        const Value *api = m ? m->metadata("apiSchemas") : nullptr;
        CHECK(api && api->list_items() == std::vector<std::string> { "MaterialBindingAPI" });
        const Property *st = m ? m->property("primvars:st") : nullptr;
        CHECK(st && st->meta.count("interpolation") && st->meta.at("interpolation").str() == "vertex");
    }
}

/* The references of a root prim, as read back */
static std::vector<std::string> refs_of(const std::map<std::string, Value> &, const Prim &root, const std::string &name,
                                        bool *isExplicit = nullptr, Specifier *spec = nullptr) {
    const Prim *p = root.child(name);
    const Value *v = p ? p->metadata("references") : nullptr;
    if (isExplicit)
        *isExplicit = v && v->isExplicit;
    if (spec && p)
        *spec = p->specifier;
    return v ? (v->isExplicit ? v->explicitItems : v->prepended) : std::vector<std::string>();
}

static void test_edit() {
    std::cout << "usd: references added in place (.usda text, .usdc appended)" << std::endl;
    const RootReference ref { "W", "./p.usda", "/W" };
    const std::string item = "./p.usda</W>";
    auto parse = [](const std::string &text, std::map<std::string, Value> &meta, Prim &root) {
        root = Prim();
        root.path = "/";
        meta.clear();
        return error_of([&] { parse_usda(text, "edit", meta, root); });
    };
    std::map<std::string, Value> meta;
    Prim root;

    /* .usda: every way the root prim may hold its references */
    struct Case { const char *before; std::vector<std::string> want; bool isExplicit; };
    const Case cases[] = {
        { "def Xform \"W\"\n{\n}\n", { item }, false },
        { "def Xform \"W\" (\n    kind = \"component\"\n)\n{\n}\n", { item }, false },
        { "def Xform \"W\" (kind = \"component\")\n{\n}\n", { item }, false },
        { "def \"W\" (\n    prepend references = @a.usda@</A> (offset = 2)\n)\n{\n}\n", { item, "a.usda</A>" }, false },
        { "def \"W\" (\n    prepend references = [@a.usda@, @b.usda@</B>]\n)\n{\n}\n",
          { item, "a.usda", "b.usda</B>" }, false },
        { "def \"W\" (\n    references = None\n)\n{\n}\n", { item }, true },
        { "def \"W\" (\n    references = [</X>]\n    append references = @c.usda@\n)\n{\n}\n", { item, "</X>" }, true },
        { "def \"W\" (\n    delete references = @c.usda@\n)\n{\n}\n", { item }, false },
    };
    for (const Case &c : cases) {
        const std::string before = std::string("#usda 1.0\n(\n    doc = \"(not { a prim\"\n)\n# def \"W\"\n") + c.before +
                                   "def \"V\" {\n    string s = \"}\"\n}\n";
        bool changed = false;
        const std::string after = usda_add_references(before, { ref }, &changed);
        bool isExplicit = false;
        CHECK(changed && parse(after, meta, root) == "");
        CHECK(refs_of(meta, root, "W", &isExplicit) == c.want && isExplicit == c.isExplicit);
        CHECK(root.child("V") && root.child("V")->property("s"));
        bool again = true;
        CHECK(usda_add_references(after, { ref }, &again) == after && !again);
    }
    /* a root prim defined in a sublayer: an over that holds the reference */
    {
        const std::string before = "#usda 1.0\n(\n    subLayers = [@./a.usda@]\n)\n";
        Specifier spec = Specifier::Def;
        CHECK(parse(usda_add_references(before, { ref }), meta, root) == "" &&
              refs_of(meta, root, "W", nullptr, &spec) == std::vector<std::string> { item } && spec == Specifier::Over);
    }

    /* .usdc: written by Pixar's USD (references prepended, explicit, none) */
    const std::string src = data_path("usd_refs.usdc"), out = temp_path("usd_refs_edit.usdc");
    const std::vector<uint8_t> original = read_file(src);
    const std::vector<RootReference> refs = { { "Prepended", "./p.usdc", "/Prepended" },
                                              { "Explicit", "./p.usdc", "/Explicit" },
                                              { "Plain", "./p.usdc", "/Plain" },
                                              { "Elsewhere", "./p.usdc", "/Elsewhere" } };
    std::vector<uint8_t> edited;
    bool changed = false;
    CHECK(error_of([&] { edited = usdc_add_references(src, refs, &changed); }) == "" && changed);
    CHECK(edited.size() > original.size() && std::equal(original.begin(), original.begin() + 16, edited.begin()) &&
          std::equal(original.begin() + 24, original.end(), edited.begin() + 24));
    write_file(out, edited);
    std::unique_ptr<Layer> back;
    CHECK(error_of([&] { back.reset(new Layer(out)); }) == "");
    if (back) {
        bool isExplicit = false;
        Specifier spec = Specifier::Def;
        const std::vector<std::string> two = { "./usd_asset.usdc</Asset>", "./usd_scene.usda</World>" };
        std::vector<std::string> want = two;
        want.insert(want.begin(), "./p.usdc</Prepended>");
        CHECK(refs_of(back->meta, back->root, "Prepended", &isExplicit) == want && !isExplicit);
        want = two;
        want.insert(want.begin(), "./p.usdc</Explicit>");
        CHECK(refs_of(back->meta, back->root, "Explicit", &isExplicit) == want && isExplicit);
        CHECK(refs_of(back->meta, back->root, "Plain") == std::vector<std::string> { "./p.usdc</Plain>" });
        CHECK(refs_of(back->meta, back->root, "Elsewhere", nullptr, &spec) ==
              std::vector<std::string> { "./p.usdc</Elsewhere>" } && spec == Specifier::Over);
        CHECK(back->root.children.size() == 4 && back->meta.count("defaultPrim") &&
              back->meta.at("defaultPrim").str() == "Prepended");
        for (const char *name : { "Prepended", "Explicit", "Plain" }) {
            const Prim *p = back->root.child(name);
            const Property *q = p ? p->property("answer") : nullptr;
            CHECK(p && p->type == "Xform" && q && back->value(*q).num() == 42);
        }
        back.reset();
        changed = true;
        std::vector<uint8_t> twice;
        CHECK(error_of([&] { twice = usdc_add_references(out, refs, &changed); }) == "" && !changed &&
              twice == edited);
    }
    std::remove(out.c_str());

    /* references written in a .usdc (the proxy layer's material stand-ins) */
    const std::string layerText = "#usda 1.0\ndef \"W\"\n{\n    def Material \"M\" (\n"
                                  "        prepend references = @./scene.usdc@</materials/m>\n    )\n    {\n    }\n"
                                  "    def \"E\" (\n        references = [@./a.usda@</A>, </W/M>]\n    )\n    {\n    }\n}\n";
    const std::string crate = temp_path("usd_refs_write.usdc");
    CHECK(error_of([&] { write_layer_file(crate, layerText, crate); }) == "");
    std::unique_ptr<Layer> written;
    CHECK(error_of([&] { written.reset(new Layer(crate)); }) == "");
    if (written) {
        const Prim *m = written->prim("/W/M"), *e = written->prim("/W/E");
        const Value *rm = m ? m->metadata("references") : nullptr, *re = e ? e->metadata("references") : nullptr;
        const std::vector<std::string> wantM = { "./scene.usdc</materials/m>" }, wantE = { "./a.usda</A>", "</W/M>" };
        CHECK(rm && !rm->isExplicit && rm->prepended == wantM);
        CHECK(re && re->isExplicit && re->explicitItems == wantE);
    }
    written.reset();
    std::remove(crate.c_str());
}

void test_usd(int fuzz_scale) {
    test_read();
    test_world();
    test_compose();
    test_write();
    test_proxy_paths();
    test_edit();
    test_fuzz(fuzz_scale);
}
