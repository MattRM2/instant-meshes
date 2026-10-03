/*
    scene.cpp: Scene files by format (see scene.h)
*/

#include "scene.h"
#include "objscene.h"
#include "usdscene.h"
#include "usdstage.h"

namespace {

std::string extension_of(const std::string &filename) {
    return filename.size() > 4 ? str_tolower(filename.substr(filename.size() - 4)) : std::string();
}

/* Alembic: read on demand, nothing kept between calls */
class AbcScene : public SceneFile {
public:
    explicit AbcScene(const std::string &filename) : SceneFile(filename) { }

    std::vector<SceneMesh> meshes() override { return abc::list_meshes(mFilename); }

    void load(const std::string &path, MatrixXu &F, MatrixXf &V, uint64_t *polygons,
              std::vector<UVSet> *uvs) override {
        abc::load_abc_mesh(mFilename, path, F, V, polygons, uvs);
    }

    void write(const std::string &output, const std::vector<SceneReplacement> &replacements) override {
        abc::splice_abc(mFilename, output, replacements);
    }
};

/* OBJ: parsed once, kept for the loads and the write */
class ObjScene : public SceneFile {
public:
    explicit ObjScene(const std::string &filename) : SceneFile(filename), mScene(filename) { }

    std::vector<SceneMesh> meshes() override {
        std::vector<SceneMesh> result;
        for (const objscene::ObjectInfo &o : mScene.objects()) {
            SceneMesh m;
            m.path = "/" + o.name;
            m.vertices = o.vertices;
            m.faces = o.faces;
            result.push_back(m);
        }
        return result;
    }

    void load(const std::string &path, MatrixXu &F, MatrixXf &V, uint64_t *polygons,
              std::vector<UVSet> *uvs) override {
        mScene.load(object_name(path), F, V, polygons, uvs);
    }

    void write(const std::string &output, const std::vector<SceneReplacement> &replacements) override {
        std::vector<objscene::Replacement> objects;
        for (const SceneReplacement &r : replacements) {
            objscene::Replacement o;
            o.name = object_name(r.path);
            o.F = r.F;
            o.V = r.V;
            o.uvs = r.uvs;
            o.fetch = r.fetch;
            objects.push_back(std::move(o));
        }
        mScene.splice(output, objects);
    }

private:
    static std::string object_name(const std::string &path) {
        return !path.empty() && path[0] == '/' ? path.substr(1) : path;
    }

    objscene::Scene mScene;
};

/* USD: the stage is composed once (attribute values read on demand); the
   output is a .usda layer over the input */
class UsdScene : public SceneFile {
public:
    explicit UsdScene(const std::string &filename)
        : SceneFile(filename), mStage(usd::open_stage(filename)), mLayer(*mStage) { }

    std::vector<SceneMesh> meshes() override { return usd::list_meshes(mLayer); }

    void load(const std::string &path, MatrixXu &F, MatrixXf &V, uint64_t *polygons,
              std::vector<UVSet> *uvs) override {
        usd::load_mesh(mLayer, path, F, V, polygons, uvs);
    }

    void write(const std::string &output, const std::vector<SceneReplacement> &replacements) override {
        usd::write_overlay(mLayer, output, replacements);
    }

    std::vector<std::string> proxy_paths(const std::vector<std::string> &meshes) override {
        return usd::proxy_paths(mLayer, meshes);
    }

    void write_proxies(const std::string &output, const std::vector<SceneReplacement> &proxies) override {
        usd::write_proxies(mLayer, output, proxies);
    }

private:
    std::shared_ptr<const usd::Layer> mStage;
    const usd::Layer &mLayer;
};

} // namespace

bool SceneFile::supported(const std::string &filename) {
    const std::string ext = extension_of(filename);
    return ext == ".abc" || ext == ".obj" || usd::is_usd_file(filename);
}

std::unique_ptr<SceneFile> SceneFile::open(const std::string &filename) {
    const std::string ext = extension_of(filename);
    if (ext == ".abc")
        return std::unique_ptr<SceneFile>(new AbcScene(filename));
    if (ext == ".obj")
        return std::unique_ptr<SceneFile>(new ObjScene(filename));
    if (usd::is_usd_file(filename))
        return std::unique_ptr<SceneFile>(new UsdScene(filename));
    throw std::runtime_error("\"" + filename + "\": not a supported scene format (.abc, .obj, .usd)!");
}
