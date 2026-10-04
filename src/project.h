/*
    project.h: an Instant Meshes project, saved as .imd. A scene file
    (referenced, not copied), the remeshing settings, and for every mesh its
    target, its state and its result. Shared by the command line (per-mesh
    mode, job.imd -o, --save-imd) and the interface (Outliner).

    .imd layout: a header (magic "IMDPROJ1", table of contents offset),
    chunks, then the table of contents (id, object index, flags, CRC-32,
    offset, stored and raw sizes). Settings chunks are readable text
    (HEAD, OPTS, OBJS, UI); results (RSLT, one per object) are binary and
    LZ4-compressed; WORK chunks hold the interface's work in progress on an
    object. Unknown chunks are skipped, so later versions can add theirs.
*/

#pragma once

#include "batch.h"
#include "scene.h"
#include "spool.h"
#include <map>
#include <memory>

enum class ObjectState { Pending = 0, Done, Skipped, Failed, Stale };

/// "pending", "done", "skipped", "failed", "stale"
const char *state_name(ObjectState s);

struct ProjectObject {
    SceneMesh mesh;
    FaceTarget target;            ///< its own target (-m); invalid: the default target
    std::string rule;             ///< the -m rule that set it ("Mesh*=75%"), empty if set by hand
    ObjectState state = ObjectState::Pending;
    std::string message;          ///< why it failed or was skipped; what was done
    Spool::Fetch result;          ///< Done / Stale: the remeshed mesh (world space) and its UV sets
    uint64_t resultFaces = 0;
    bool checked = false;         ///< selected in the Outliner
    std::vector<uint8_t> work;    ///< work in progress of the interface (strokes...), kept as is
};

struct ProjectOptions {
    RemeshParams params;
    FaceTarget others;            ///< target of the meshes without their own (--others)
    bool proxy = false;           ///< USD: add the results as proxies (--proxy)
    bool skipFailed = false;      ///< a mesh that fails is kept unchanged (--skip-failed)
};

class Project {
public:
    std::string source;           ///< the scene file
    ProjectOptions options;
    std::vector<ProjectObject> objects;
    std::map<std::string, std::string> ui;   ///< interface state (Outliner, camera), kept as is
    std::string output;           ///< the last scene written

    /// A project over a scene file: its meshes listed, nothing planned
    static std::unique_ptr<Project> create(const std::string &scene);

    /**
     * Opens a .imd. 'source', when given, replaces the scene path it records
     * (the scene was moved). If the scene changed since, the meshes whose
     * face or vertex count changed lose their result (state Stale), the
     * meshes it no longer has are dropped, the new ones added.
     */
    static std::unique_ptr<Project> load(const std::string &imd, const std::string &source = std::string());

    /// Writes the project (atomically); results are read back from it afterwards
    void save(const std::string &imd);

    ~Project();

    /// The scene, opened on first use
    SceneFile &scene();

    /// -m rules: each mesh gets the target of the last rule that matches it.
    /// Throws (as the command line does) for a rule that matches nothing or
    /// a mesh that cannot be remeshed
    void apply_rules(const std::vector<MeshRule> &rules);

    /// Animated, instanced, or (proxy mode) a proxy or guide mesh
    bool unfit(const ProjectObject &o) const;
    /// The target of a mesh: its own, else the default one; invalid: kept unchanged
    FaceTarget target_of(const ProjectObject &o) const;
    /// Why: "-m <rule>", "--others <target>", "kept unchanged (flags)"
    std::string reason_of(const ProjectObject &o) const;

    /// Remeshes one mesh with its target: state Done and its result kept
    /// (temporary file), or Failed with the error, rethrown
    RemeshReport process(ProjectObject &o);

    /// Writes the scene with the results of the done meshes (replaced, or
    /// added as proxies); throws if none
    void write(const std::string &output);

    /// Temporary file of the results computed in this session (default:
    /// next to the project or the scene)
    void set_spool(const std::string &path);

    /// The project file results are read from (empty for a new project)
    const std::string &file() const { return mFile; }

private:
    std::unique_ptr<SceneFile> mScene;
    std::unique_ptr<Spool> mSpool;
    std::string mSpoolPath, mFile;
};

/// Flags of a mesh as --list prints them: " (animated)", " (instanced)", " (proxy)"
std::string mesh_flags(const SceneMesh &m);
