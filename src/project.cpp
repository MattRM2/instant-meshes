/*
    project.cpp: Instant Meshes projects and the .imd format (see project.h)
*/

#if defined(_WIN32)
#  if !defined(NOMINMAX)
#    define NOMINMAX
#  endif
#  if !defined(WIN32_LEAN_AND_MEAN)
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#endif

#include "project.h"
#include "usdc.h"
#include "usdcwrite.h"
#include "version.h"
#include <sys/stat.h>
#include <cstring>
#include <fstream>
#include <set>
#include <sstream>

using usd::crc32;
using usd::lz4_block;
using usd::lz4_compress;

const char *state_name(ObjectState s) {
    switch (s) {
        case ObjectState::Done: return "done";
        case ObjectState::Skipped: return "skipped";
        case ObjectState::Failed: return "failed";
        case ObjectState::Stale: return "stale";
        default: return "pending";
    }
}

std::string mesh_flags(const SceneMesh &m) {
    std::string flags;
    if (m.animated)
        flags += " (animated)";
    if (m.instanced)
        flags += " (instanced)";
    if (!m.purpose.empty())
        flags += " (" + m.purpose + ")";
    return flags;
}

namespace {

/* ---- files ---- */

bool file_info(const std::string &path, uint64_t &size, int64_t &mtime) {
#if defined(_WIN32)
    struct _stat64 st;
    if (_stat64(path.c_str(), &st) != 0)
        return false;
#else
    struct stat st;
    if (stat(path.c_str(), &st) != 0)
        return false;
#endif
    size = (uint64_t) st.st_size;
    mtime = (int64_t) st.st_mtime;
    return true;
}

bool exists(const std::string &path) {
    uint64_t size;
    int64_t mtime;
    return !path.empty() && file_info(path, size, mtime);
}

std::string absolute(const std::string &path) {
#if defined(_WIN32)
    char buf[4096];
    if (_fullpath(buf, path.c_str(), sizeof buf))
        return buf;
#else
    char buf[4096];
    if (realpath(path.c_str(), buf))
        return buf;
#endif
    return path;
}

std::string dir_of(const std::string &p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string() : p.substr(0, s + 1);
}

std::string base_of(const std::string &p) {
    const size_t s = p.find_last_of("/\\");
    return s == std::string::npos ? p : p.substr(s + 1);
}

void replace_file(const std::string &temp, const std::string &target) {
#if defined(_WIN32)
    const bool moved = MoveFileExA(temp.c_str(), target.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    const bool moved = std::rename(temp.c_str(), target.c_str()) == 0;
#endif
    if (!moved) {
        std::remove(temp.c_str());
        throw std::runtime_error("Unable to replace \"" + target + "\" (is it open in another program?)!");
    }
}

/* ---- text chunks: key=value lines, tab-separated records ---- */

std::string escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '\t': out += "\\t"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            default: out += c;
        }
    }
    return out;
}

std::string unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        out += c == 't' ? '\t' : c == 'n' ? '\n' : c == 'r' ? '\r' : c;
    }
    return out;
}

std::vector<std::string> split(const std::string &line, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        const size_t e = line.find(sep, start);
        out.push_back(line.substr(start, e == std::string::npos ? std::string::npos : e - start));
        if (e == std::string::npos)
            break;
        start = e + 1;
    }
    return out;
}

std::vector<uint8_t> text_bytes(const std::map<std::string, std::string> &kv) {
    std::string t;
    for (const auto &e : kv)
        t += e.first + "=" + escape(e.second) + "\n";
    return std::vector<uint8_t>(t.begin(), t.end());
}

std::map<std::string, std::string> parse_text(const std::vector<uint8_t> &b) {
    std::map<std::string, std::string> kv;
    for (const std::string &line : split(std::string(b.begin(), b.end()), '\n')) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos)
            kv[line.substr(0, eq)] = unescape(line.substr(eq + 1));
    }
    return kv;
}

/* ---- results: rows, faces, vertices, F, V, then each UV set ---- */

template <typename T> void put(std::vector<uint8_t> &b, const T &v) {
    const size_t at = b.size();
    b.resize(at + sizeof(T));
    memcpy(b.data() + at, &v, sizeof(T));
}

void put_bytes(std::vector<uint8_t> &b, const void *data, size_t size) {
    b.insert(b.end(), (const uint8_t *) data, (const uint8_t *) data + size);
}

std::vector<uint8_t> serialize(const MatrixXu &F, const MatrixXf &V, const std::vector<CornerUVs> &uvs) {
    std::vector<uint8_t> b;
    put<uint32_t>(b, (uint32_t) F.rows());
    put<uint64_t>(b, (uint64_t) F.cols());
    put<uint64_t>(b, (uint64_t) V.cols());
    put_bytes(b, F.data(), sizeof(uint32_t) * (size_t) F.size());
    put_bytes(b, V.data(), sizeof(float) * (size_t) V.size());
    put<uint32_t>(b, (uint32_t) uvs.size());
    for (const CornerUVs &s : uvs) {
        put<uint32_t>(b, (uint32_t) s.name.size());
        put_bytes(b, s.name.data(), s.name.size());
        put<uint64_t>(b, (uint64_t) s.corners.cols());
        put_bytes(b, s.corners.data(), sizeof(float) * (size_t) s.corners.size());
    }
    return b;
}

void deserialize(const std::vector<uint8_t> &b, MatrixXu &F, MatrixXf &V, std::vector<CornerUVs> &uvs) {
    size_t pos = 0;
    auto take = [&](void *out, size_t size) {
        if (size > b.size() - pos)
            throw std::runtime_error("Instant Meshes project: truncated result!");
        memcpy(out, b.data() + pos, size);
        pos += size;
    };
    uint32_t rows, count;
    uint64_t faces, vertices;
    take(&rows, 4);
    take(&faces, 8);
    take(&vertices, 8);
    if (rows < 3 || rows > 4 || faces > b.size() || vertices > b.size())
        throw std::runtime_error("Instant Meshes project: invalid result!");
    F.resize(rows, (std::ptrdiff_t) faces);
    V.resize(3, (std::ptrdiff_t) vertices);
    take(F.data(), sizeof(uint32_t) * (size_t) F.size());
    take(V.data(), sizeof(float) * (size_t) V.size());
    take(&count, 4);
    if (count > 64)
        throw std::runtime_error("Instant Meshes project: invalid result!");
    uvs.resize(count);
    for (CornerUVs &s : uvs) {
        uint32_t n;
        uint64_t corners;
        take(&n, 4);
        if (n > b.size())
            throw std::runtime_error("Instant Meshes project: invalid result!");
        s.name.resize(n);
        take(&s.name[0], n);
        take(&corners, 8);
        if (corners > b.size())
            throw std::runtime_error("Instant Meshes project: invalid result!");
        s.corners.resize(2, (std::ptrdiff_t) corners);
        take(s.corners.data(), sizeof(float) * (size_t) s.corners.size());
    }
    for (uint32_t i = 0; i < (uint32_t) F.size(); ++i)
        if (F.data()[i] >= vertices)
            throw std::runtime_error("Instant Meshes project: invalid result!");
}

/* ---- chunk file ---- */

const char Magic[8] = { 'I', 'M', 'D', 'P', 'R', 'O', 'J', '1' };
const uint32_t FormatVersion = 1, FlagLZ4 = 1;

struct Chunk {
    char id[4] = { 0, 0, 0, 0 };
    uint32_t index = 0, flags = 0, crc = 0;
    uint64_t offset = 0, stored = 0, raw = 0;

    std::string name() const { return std::string(id, strnlen(id, 4)); }
};

std::vector<uint8_t> read_chunk(const std::string &file, const Chunk &c) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        throw std::runtime_error("Unable to open \"" + file + "\"!");
    std::vector<uint8_t> stored((size_t) c.stored);
    in.seekg((std::streamoff) c.offset);
    if (c.stored > 0)
        in.read((char *) stored.data(), (std::streamsize) c.stored);
    if (!in)
        throw std::runtime_error("\"" + file + "\": truncated project file!");
    std::vector<uint8_t> raw;
    if (c.flags & FlagLZ4) {
        raw.resize((size_t) c.raw);
        const size_t n = lz4_block(stored.data(), stored.size(), raw.data(), raw.size());
        if (n != c.raw)
            throw std::runtime_error("\"" + file + "\": corrupted chunk " + c.name() + "!");
    } else {
        raw.swap(stored);
    }
    if (crc32(raw.data(), raw.size()) != c.crc)
        throw std::runtime_error("\"" + file + "\": corrupted chunk " + c.name() + " (checksum)!");
    return raw;
}

std::vector<Chunk> read_toc(const std::string &file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in)
        throw std::runtime_error("Unable to open \"" + file + "\"!");
    const uint64_t size = (uint64_t) in.tellg();
    in.seekg(0);
    char magic[8];
    uint32_t version, reserved;
    uint64_t toc;
    in.read(magic, 8);
    in.read((char *) &version, 4);
    in.read((char *) &reserved, 4);
    in.read((char *) &toc, 8);
    if (!in || memcmp(magic, Magic, 8) != 0)
        throw std::runtime_error("\"" + file + "\": not an Instant Meshes project (.imd)!");
    if (version > FormatVersion)
        throw std::runtime_error("\"" + file + "\": made by a newer Instant Meshes (format " +
                                 std::to_string(version) + "), please update!");
    if (toc > size - 4)
        throw std::runtime_error("\"" + file + "\": truncated project file!");
    in.seekg((std::streamoff) toc);
    uint32_t count;
    in.read((char *) &count, 4);
    if (!in || (uint64_t) count * 40 > size - toc - 4)
        throw std::runtime_error("\"" + file + "\": corrupted table of contents!");
    std::vector<Chunk> chunks(count);
    for (Chunk &c : chunks) {
        in.read(c.id, 4);
        in.read((char *) &c.index, 4);
        in.read((char *) &c.flags, 4);
        in.read((char *) &c.crc, 4);
        in.read((char *) &c.offset, 8);
        in.read((char *) &c.stored, 8);
        in.read((char *) &c.raw, 8);
        if (!in || c.offset > toc || c.stored > toc - c.offset || c.raw > ((uint64_t) 1 << 40))
            throw std::runtime_error("\"" + file + "\": corrupted table of contents!");
    }
    return chunks;
}

Spool::Fetch chunk_fetch(const std::string &file, const Chunk &c) {
    return [file, c](MatrixXu &F, MatrixXf &V, std::vector<CornerUVs> &uvs) {
        deserialize(read_chunk(file, c), F, V, uvs);
    };
}

class ChunkWriter {
public:
    explicit ChunkWriter(const std::string &path) : mPath(path), mOut(path, std::ios::binary | std::ios::trunc) {
        if (!mOut)
            throw std::runtime_error("Unable to create \"" + path + "\"!");
        char header[24] = { 0 };
        memcpy(header, Magic, 8);
        memcpy(header + 8, &FormatVersion, 4);
        mOut.write(header, 24);
        mPos = 24;
    }

    const Chunk &add(const char *id, uint32_t index, const std::vector<uint8_t> &raw, bool compress) {
        Chunk c;
        memcpy(c.id, id, std::min<size_t>(4, strlen(id)));
        c.index = index;
        c.crc = crc32(raw.data(), raw.size());
        c.raw = raw.size();
        c.offset = mPos;
        if (compress && raw.size() > 64) {
            const std::vector<uint8_t> z = lz4_compress(raw.data(), raw.size());
            if (z.size() < raw.size()) {
                c.flags |= FlagLZ4;
                write(z.data(), z.size());
                c.stored = z.size();
            }
        }
        if (!(c.flags & FlagLZ4)) {
            write(raw.data(), raw.size());
            c.stored = raw.size();
        }
        mChunks.push_back(c);
        return mChunks.back();
    }

    const std::vector<Chunk> &finish() {
        const uint64_t toc = mPos;
        const uint32_t count = (uint32_t) mChunks.size();
        write(&count, 4);
        for (const Chunk &c : mChunks) {
            write(c.id, 4);
            write(&c.index, 4);
            write(&c.flags, 4);
            write(&c.crc, 4);
            write(&c.offset, 8);
            write(&c.stored, 8);
            write(&c.raw, 8);
        }
        mOut.seekp(16);
        mOut.write((const char *) &toc, 8);
        mOut.flush();
        if (!mOut)
            throw std::runtime_error("Error while writing \"" + mPath + "\" (disk full?)!");
        mOut.close();
        return mChunks;
    }

private:
    void write(const void *data, size_t size) {
        mOut.write((const char *) data, (std::streamsize) size);
        if (!mOut)
            throw std::runtime_error("Error while writing \"" + mPath + "\" (disk full?)!");
        mPos += size;
    }

    std::string mPath;
    std::ofstream mOut;
    uint64_t mPos = 0;
    std::vector<Chunk> mChunks;
};

/* ---- settings ---- */

std::string target_text(const FaceTarget &t) {
    return t.valid() ? t.text : std::string();
}

FaceTarget parse_target(const std::string &text) {
    return text.empty() ? FaceTarget() : parse_face_target(text);
}

const char *uv_name(RemeshParams::UVMode m) {
    return m == RemeshParams::UVTransfer ? "transfer" : m == RemeshParams::UVUnwrap ? "unwrap" : "none";
}

std::map<std::string, std::string> options_text(const ProjectOptions &o) {
    const RemeshParams &p = o.params;
    std::map<std::string, std::string> kv;
    kv["rosy"] = std::to_string(p.rosy);
    kv["posy"] = std::to_string(p.posy);
    kv["crease"] = std::to_string(p.crease_angle);
    kv["extrinsic"] = p.extrinsic ? "1" : "0";
    kv["boundaries"] = p.align_to_boundaries ? "1" : "0";
    kv["smooth"] = std::to_string(p.smooth_iter);
    kv["pure_quad"] = p.pure_quad ? "1" : "0";
    kv["deterministic"] = p.deterministic ? "1" : "0";
    kv["keep_border"] = p.keep_border ? "1" : "0";
    kv["uv"] = uv_name(p.uv);
    kv["others"] = target_text(o.others);
    kv["proxy"] = o.proxy ? "1" : "0";
    kv["skip_failed"] = o.skipFailed ? "1" : "0";
    return kv;
}

void parse_options(const std::map<std::string, std::string> &kv, ProjectOptions &o) {
    auto get = [&](const char *key, const std::string &fallback) {
        auto it = kv.find(key);
        return it == kv.end() ? fallback : it->second;
    };
    RemeshParams &p = o.params;
    p.rosy = std::stoi(get("rosy", "4"));
    p.posy = std::stoi(get("posy", "4"));
    if ((p.rosy != 2 && p.rosy != 4 && p.rosy != 6) || (p.posy != 3 && p.posy != 4))
        throw std::runtime_error("Instant Meshes project: invalid symmetry!");
    p.crease_angle = std::stof(get("crease", "-1"));
    p.extrinsic = get("extrinsic", "1") == "1";
    p.align_to_boundaries = get("boundaries", "0") == "1";
    p.smooth_iter = std::stoi(get("smooth", "2"));
    p.pure_quad = get("pure_quad", "1") == "1";
    p.deterministic = get("deterministic", "0") == "1";
    p.keep_border = get("keep_border", "0") == "1";
    p.uv = parse_uv_mode(get("uv", "none"));
    o.others = parse_target(get("others", ""));
    o.proxy = get("proxy", "0") == "1";
    o.skipFailed = get("skip_failed", "0") == "1";
}

} // namespace

/* ---- Project ---- */

Project::~Project() { }

std::unique_ptr<Project> Project::create(const std::string &scene) {
    std::unique_ptr<Project> p(new Project());
    p->source = scene;
    p->mScene = SceneFile::open(scene);
    for (const SceneMesh &m : p->mScene->meshes()) {
        ProjectObject o;
        o.mesh = m;
        p->objects.push_back(std::move(o));
    }
    return p;
}

SceneFile &Project::scene() {
    if (!mScene)
        mScene = SceneFile::open(source);
    return *mScene;
}

void Project::set_spool(const std::string &path) {
    mSpoolPath = path;
}

bool Project::unfit(const ProjectObject &o) const {
    return o.mesh.animated || o.mesh.instanced ||
           (options.proxy && (o.mesh.purpose == "proxy" || o.mesh.purpose == "guide"));
}

FaceTarget Project::target_of(const ProjectObject &o) const {
    if (o.target.valid())
        return o.target;
    if (options.others.valid() && !unfit(o))
        return options.others;
    return FaceTarget();
}

std::string Project::reason_of(const ProjectObject &o) const {
    if (o.target.valid())
        return "-m " + (o.rule.empty() ? o.mesh.path.substr(o.mesh.path[0] == '/' ? 1 : 0) + "=" + o.target.text
                                       : o.rule);
    if (options.others.valid())
        return unfit(o) ? "kept unchanged" + mesh_flags(o.mesh) : "--others " + options.others.text;
    return "kept unchanged";
}

void Project::apply_rules(const std::vector<MeshRule> &rules) {
    std::vector<size_t> matches(rules.size(), 0);
    std::vector<std::string> refused;
    for (ProjectObject &o : objects) {
        int rule = -1;
        for (size_t r = 0; r < rules.size(); ++r) {
            if (rule_matches(rules[r].pattern, o.mesh.path)) {
                rule = (int) r;
                matches[r]++;
            }
        }
        if (rule < 0)
            continue;
        o.target = rules[(size_t) rule].target;
        o.rule = rules[(size_t) rule].text;
        if (unfit(o))
            refused.push_back(o.mesh.path + mesh_flags(o.mesh) + ", selected by -m " + o.rule);
    }
    for (size_t r = 0; r < rules.size(); ++r)
        if (matches[r] == 0)
            throw std::runtime_error("-m " + rules[r].text + ": no polygon mesh matches \"" +
                                     rules[r].pattern + "\" (see --list)");
    if (!refused.empty()) {
        std::string list;
        for (const std::string &s : refused)
            list += "\n   " + s;
        throw std::runtime_error(options.proxy ? "Animated, instanced, proxy or guide meshes cannot get a proxy:" + list
                                               : "Animated or instanced meshes cannot be remeshed:" + list);
    }
}

RemeshReport Project::process(ProjectObject &o) {
    const FaceTarget t = target_of(o);
    if (!t.valid())
        throw std::runtime_error("\"" + o.mesh.path + "\" has no target");
    try {
        ObjectResult r = remesh_object(scene(), o.mesh.path, t, options.params);
        if (!mSpool)
            mSpool.reset(new Spool(!mSpoolPath.empty() ? mSpoolPath
                                   : (mFile.empty() ? source : mFile) + ".spool.tmp"));
        o.result = mSpool->put(r.F, r.V, r.uvs);
        o.resultFaces = (uint64_t) r.F.cols();
        o.state = ObjectState::Done;
        o.message.clear();
        return r.report;
    } catch (const std::exception &e) {
        o.state = ObjectState::Failed;
        o.message = e.what();
        throw;
    }
}

void Project::write(const std::string &out) {
    std::vector<abc::Replacement> replacements;
    for (const ProjectObject &o : objects) {
        if (o.state != ObjectState::Done || !o.result)
            continue;
        abc::Replacement r;
        r.path = o.mesh.path;
        r.fetch = o.result;
        replacements.push_back(std::move(r));
    }
    if (options.proxy) {
        if (replacements.empty())
            throw std::runtime_error("No proxy could be made, nothing written!");
        scene().write_proxies(out, replacements);
    } else {
        scene().write(out, replacements);   /* none remeshed: an unchanged copy */
    }
    output = out;
}

void Project::save(const std::string &imd) {
    const std::string temp = imd + ".tmp";
    struct TempGuard {
        const std::string &path;
        bool armed = true;
        ~TempGuard() { if (armed) std::remove(path.c_str()); }
    } guard { temp };

    std::vector<std::pair<size_t, Chunk>> results;
    {
        ChunkWriter w(temp);
        std::map<std::string, std::string> head;
        head["format"] = std::to_string(FormatVersion);
        head["app"] = INSTANT_MESHES_TITLE;
        const std::string src = absolute(source);
        head["source"] = src;
        const std::string imdDir = dir_of(absolute(imd));
        if (!imdDir.empty() && str_tolower(dir_of(src)).compare(0, imdDir.size(), str_tolower(imdDir)) == 0)
            head["source_rel"] = "./" + src.substr(imdDir.size());
        uint64_t size = 0;
        int64_t mtime = 0;
        if (file_info(source, size, mtime)) {
            head["source_size"] = std::to_string(size);
            head["source_mtime"] = std::to_string(mtime);
        }
        if (!output.empty())
            head["output"] = output;
        w.add("HEAD", 0, text_bytes(head), false);
        w.add("OPTS", 0, text_bytes(options_text(options)), false);

        std::string objs;
        for (const ProjectObject &o : objects)
            objs += escape(o.mesh.path) + "\t" + std::to_string(o.mesh.faces) + "\t" +
                    std::to_string(o.mesh.vertices) + "\t" + target_text(o.target) + "\t" + escape(o.rule) + "\t" +
                    state_name(o.state) + "\t" + escape(o.message) + "\t" + (o.checked ? "1" : "0") + "\t" +
                    std::to_string(o.resultFaces) + "\n";
        w.add("OBJS", 0, std::vector<uint8_t>(objs.begin(), objs.end()), false);

        for (size_t i = 0; i < objects.size(); ++i) {
            const ProjectObject &o = objects[i];
            if (o.result && (o.state == ObjectState::Done || o.state == ObjectState::Stale)) {
                MatrixXu F;
                MatrixXf V;
                std::vector<CornerUVs> uvs;
                o.result(F, V, uvs);
                results.emplace_back(i, w.add("RSLT", (uint32_t) i, serialize(F, V, uvs), true));
            }
            if (!o.work.empty())
                w.add("WORK", (uint32_t) i, o.work, true);
        }
        if (!ui.empty())
            w.add("UI", 0, text_bytes(ui), false);
        w.finish();
    }
    replace_file(temp, imd);
    guard.armed = false;
    /* the results now come from the project file */
    mFile = imd;
    for (const auto &r : results)
        objects[r.first].result = chunk_fetch(imd, r.second);
}

std::unique_ptr<Project> Project::load(const std::string &imd, const std::string &sourceOverride) {
    const std::vector<Chunk> chunks = read_toc(imd);
    auto chunk = [&](const char *id) -> const Chunk * {
        for (const Chunk &c : chunks)
            if (c.name() == id)
                return &c;
        return nullptr;
    };
    const Chunk *headChunk = chunk("HEAD"), *optsChunk = chunk("OPTS"), *objsChunk = chunk("OBJS");
    if (!headChunk || !optsChunk || !objsChunk)
        throw std::runtime_error("\"" + imd + "\": incomplete project file!");
    const std::map<std::string, std::string> head = parse_text(read_chunk(imd, *headChunk));

    /* the scene: where it was, next to the project, or where the user says */
    std::string source = sourceOverride;
    if (source.empty()) {
        auto at = [&](const char *key) {
            auto it = head.find(key);
            return it == head.end() ? std::string() : it->second;
        };
        const std::string dir = dir_of(absolute(imd));
        for (const std::string &candidate : { at("source"), at("source_rel").empty() ? std::string()
                                                                                : dir + at("source_rel").substr(2),
                                              dir + base_of(at("source")) })
            if (exists(candidate)) {
                source = candidate;
                break;
            }
        if (source.empty())
            throw std::runtime_error("The scene of the project \"" + imd + "\", \"" + at("source") +
                                     "\", cannot be found (moved or deleted?)");
    }
    std::unique_ptr<Project> p = create(source);
    p->mFile = imd;
    parse_options(parse_text(read_chunk(imd, *optsChunk)), p->options);
    auto out = head.find("output");
    if (out != head.end())
        p->output = out->second;

    /* has the scene changed since? */
    bool changed = false;
    {
        uint64_t size = 0;
        int64_t mtime = 0;
        file_info(p->source, size, mtime);
        auto s = head.find("source_size"), t = head.find("source_mtime");
        changed = s == head.end() || t == head.end() || s->second != std::to_string(size) ||
                  t->second != std::to_string(mtime);
    }

    std::map<std::string, size_t> index;
    for (size_t i = 0; i < p->objects.size(); ++i)
        index[p->objects[i].mesh.path] = i;
    std::map<uint32_t, size_t> recordOf;   /* object index in the file -> in the project */
    const std::vector<uint8_t> objs = read_chunk(imd, *objsChunk);
    uint32_t record = 0, dropped = 0;
    for (const std::string &line : split(std::string(objs.begin(), objs.end()), '\n')) {
        if (line.empty())
            continue;
        const std::vector<std::string> f = split(line, '\t');
        const uint32_t self = record++;
        if (f.size() < 9)
            throw std::runtime_error("\"" + imd + "\": corrupted object list!");
        auto it = index.find(unescape(f[0]));
        if (it == index.end()) {
            ++dropped;
            continue;
        }
        ProjectObject &o = p->objects[it->second];
        o.target = parse_target(f[3]);
        o.rule = unescape(f[4]);
        for (ObjectState s : { ObjectState::Pending, ObjectState::Done, ObjectState::Skipped, ObjectState::Failed,
                               ObjectState::Stale })
            if (f[5] == state_name(s))
                o.state = s;
        o.message = unescape(f[6]);
        o.checked = f[7] == "1";
        o.resultFaces = std::stoull(f[8]);
        if (changed && (std::to_string(o.mesh.faces) != f[1] || std::to_string(o.mesh.vertices) != f[2]) &&
            (o.state == ObjectState::Done || o.state == ObjectState::Stale)) {
            o.state = ObjectState::Stale;
            o.message = "changed in the scene since it was remeshed";
        }
        recordOf[self] = it->second;
    }
    for (const Chunk &c : chunks) {
        auto r = recordOf.find(c.index);
        if (r == recordOf.end())
            continue;
        ProjectObject &o = p->objects[r->second];
        if (c.name() == "RSLT")
            o.result = chunk_fetch(imd, c);
        else if (c.name() == "WORK")
            o.work = read_chunk(imd, c);
    }
    for (ProjectObject &o : p->objects)
        if (o.state == ObjectState::Done && !o.result)
            o.state = ObjectState::Pending;
    if (const Chunk *u = chunk("UI"))
        p->ui = parse_text(read_chunk(imd, *u));
    if (dropped > 0)
        cout << "Warning: " << dropped << " mesh" << (dropped > 1 ? "es" : "")
             << " of the project no longer in the scene, dropped" << endl;
    return p;
}
