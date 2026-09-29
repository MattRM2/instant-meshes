/*
    test_ogawa.cpp -- Unit tests for the Ogawa container reader/writer:
    reference files, round-trip copies, crafted corruptions and fuzzing.
*/

#include "test_common.h"
#include "ogawa.h"
#include <pcg32.h>
#include <chrono>
#include <map>

static const char *REFERENCE_FILES[] = {
    "cube_quads.abc", "ngon_cylinder.abc", "suzanne_open.abc",
    "hierarchy.abc", "scene_ab.abc", "animated.abc"
};

/* Structural equality of two DAGs: same shape, same data bytes */
static bool same_tree(ogawa::Reader &a, uint64_t ea, ogawa::Reader &b, uint64_t eb,
                      std::map<std::pair<uint64_t, uint64_t>, bool> &memo) {
    if (ogawa::is_data(ea) != ogawa::is_data(eb))
        return false;
    auto key = std::make_pair(ea, eb);
    auto it = memo.find(key);
    if (it != memo.end())
        return it->second;
    bool same;
    if (ogawa::is_data(ea)) {
        same = a.data(ea) == b.data(eb);
    } else {
        std::vector<uint64_t> ca = a.group(ea), cb = b.group(eb);
        same = ca.size() == cb.size();
        for (size_t i = 0; same && i < ca.size(); ++i)
            same = same_tree(a, ca[i], b, cb[i], memo);
    }
    memo[key] = same;
    return same;
}

/* ------------------------------------------------------------------------- */
/*  Crafted files                                                            */
/* ------------------------------------------------------------------------- */

struct Blob {
    std::vector<uint8_t> bytes;

    explicit Blob(uint8_t frozen = 0xff, uint16_t version = 1) {
        const char magic[] = "Ogawa";
        bytes.assign(magic, magic + 5);
        bytes.push_back(frozen);
        bytes.push_back((uint8_t) (version >> 8));
        bytes.push_back((uint8_t) version);
        u64(0);   /* root, patched by set_root() */
    }
    uint64_t pos() const { return bytes.size(); }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i)
            bytes.push_back((uint8_t) (v >> (8 * i)));
    }
    void patch(uint64_t at, uint64_t v) {
        for (int i = 0; i < 8; ++i)
            bytes[at + i] = (uint8_t) (v >> (8 * i));
    }
    uint64_t data(const std::string &payload) {
        const uint64_t p = pos();
        u64(payload.size());
        bytes.insert(bytes.end(), payload.begin(), payload.end());
        return p | ogawa::DATA_BIT;
    }
    uint64_t group(const std::vector<uint64_t> &children) {
        const uint64_t p = pos();
        u64(children.size());
        for (uint64_t c : children)
            u64(c);
        return p;
    }
    void set_root(uint64_t root) { patch(8, root); }
};

static std::string scan_error(const std::vector<uint8_t> &bytes, const char *name) {
    const std::string path = temp_path(name);
    write_file(path, bytes);
    return error_of([&] {
        ogawa::Reader in(path);
        ogawa::scan(in);
    });
}

static void test_crafted() {
    std::cout << "ogawa: crafted corruptions" << std::endl;

    /* Sanity: a minimal valid archive */
    {
        Blob b;
        uint64_t d = b.data("hello");
        b.set_root(b.group({d, ogawa::EMPTY_DATA, ogawa::EMPTY_GROUP}));
        CHECK(scan_error(b.bytes, "ok.abc") == "");
    }

    struct Case { const char *name; std::vector<uint8_t> bytes; const char *expect; };
    std::vector<Case> cases;

    cases.push_back({"small.abc", {'O', 'g', 'a'}, "too small"});
    {
        std::vector<uint8_t> hdf = {0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
        hdf.resize(64, 0);
        cases.push_back({"hdf5.abc", hdf, "HDF5"});
    }
    {
        Blob b; b.set_root(b.group({}));
        b.bytes[0] = 'X';
        cases.push_back({"magic.abc", b.bytes, "not an Alembic"});
    }
    {
        Blob b(0x00); b.set_root(b.group({b.data("x")}));
        cases.push_back({"unfrozen.abc", b.bytes, "never finalized"});
    }
    {
        Blob b(0xff, 2); b.set_root(b.group({}));
        cases.push_back({"version.abc", b.bytes, "version 2"});
    }
    {
        Blob b; b.group({});   /* root left at 0 */
        cases.push_back({"noroot.abc", b.bytes, "invalid root"});
    }
    {
        Blob b; b.set_root(b.data("x"));
        cases.push_back({"rootdata.abc", b.bytes, "invalid root"});
    }
    {
        Blob b; b.set_root(1u << 20);
        cases.push_back({"rootfar.abc", b.bytes, "invalid position"});
    }
    {
        Blob b; uint64_t g = b.group({});
        b.patch(g, 0x0fffffffffffffffULL);   /* absurd child count */
        b.set_root(g);
        cases.push_back({"count.abc", b.bytes, "child count"});
    }
    {
        Blob b; b.set_root(b.group({(1u << 20) | ogawa::DATA_BIT}));
        cases.push_back({"childfar.abc", b.bytes, "invalid position"});
    }
    {
        Blob b; b.set_root(b.group({5}));   /* child inside the header */
        cases.push_back({"childheader.abc", b.bytes, "invalid position"});
    }
    {
        Blob b; uint64_t d = b.data("abc");
        b.patch(ogawa::entry_pos(d), 1u << 30);   /* size larger than file */
        b.set_root(b.group({d}));
        cases.push_back({"datasize.abc", b.bytes, "size exceeds"});
    }
    {
        Blob b; uint64_t g = b.group({0});
        b.patch(g + 8, g);   /* group containing itself */
        b.set_root(g);
        cases.push_back({"selfcycle.abc", b.bytes, "cyclic"});
    }
    {
        Blob b; uint64_t a = b.group({0});
        uint64_t c = b.group({a});
        b.patch(a + 8, c);   /* a -> c -> a */
        b.set_root(c);
        cases.push_back({"cycle.abc", b.bytes, "cyclic"});
    }
    {
        Blob b; uint64_t g = b.group({});
        for (int i = 0; i < 2000; ++i)
            g = b.group({g});
        b.set_root(g);
        cases.push_back({"deep.abc", b.bytes, "nested deeper"});
    }

    for (const Case &c : cases) {
        const std::string msg = scan_error(c.bytes, c.name);
        const bool ok = contains(msg, c.expect);
        if (!ok)
            std::cerr << "  " << c.name << ": got \"" << msg << "\"" << std::endl;
        CHECK(ok);
    }

    /* "Billion laughs": 64 levels, each group references the next one twice
       -> 2^64 paths. Memoization must make this instant and valid. */
    {
        Blob b; uint64_t g = b.group({b.data("leaf")});
        for (int i = 0; i < 64; ++i)
            g = b.group({g, g});
        b.set_root(g);
        write_file(temp_path("laughs.abc"), b.bytes);
        auto t0 = std::chrono::steady_clock::now();
        ogawa::Stats s;
        std::string msg = error_of([&] {
            ogawa::Reader in(temp_path("laughs.abc"));
            s = ogawa::scan(in);
        });
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        CHECK(msg == "" && s.groups == 65 && s.datas == 1 && ms < 1000);

        /* ...and the copy keeps the sharing (same size, not 2^64 blocks) */
        msg = error_of([&] {
            ogawa::Reader in(temp_path("laughs.abc"));
            ogawa::Writer out(temp_path("laughs_copy.abc"));
            out.commit(ogawa::copy(in, out, in.root()));
        });
        CHECK(msg == "" && read_file(temp_path("laughs_copy.abc")).size() == b.bytes.size());
    }
}

/* ------------------------------------------------------------------------- */
/*  Reference files and round trips                                          */
/* ------------------------------------------------------------------------- */

static void test_reference_files() {
    std::cout << "ogawa: reference files + round-trip copies" << std::endl;
    for (const char *name : REFERENCE_FILES) {
        ogawa::Stats s, s2;
        bool same = false;
        const std::string copy = temp_path(std::string("copy_") + name);
        const std::string msg = error_of([&] {
            ogawa::Reader in(data_path(name));
            s = ogawa::scan(in);
            {
                ogawa::Writer out(copy);
                out.commit(ogawa::copy(in, out, in.root()));
            }
            ogawa::Reader in2(copy);
            s2 = ogawa::scan(in2);
            std::map<std::pair<uint64_t, uint64_t>, bool> memo;
            same = same_tree(in, in.root(), in2, in2.root(), memo);
        });
        if (msg != "")
            std::cerr << "  " << name << ": " << msg << std::endl;
        CHECK(msg == "");
        CHECK(s.groups > 0 && s.datas > 0);
        CHECK(same);
        CHECK(s.groups == s2.groups && s.datas == s2.datas &&
              s.dataBytes == s2.dataBytes && s.references == s2.references &&
              s.maxDepth == s2.maxDepth);
        CHECK(!file_exists(copy + ".tmp"));
    }
}

static void test_writer_safety() {
    std::cout << "ogawa: writer safety" << std::endl;
    const std::string target = temp_path("keep.abc");
    write_file(target, std::string("ORIGINAL"));

    /* Abandoned write: target untouched, temporary file removed */
    {
        ogawa::Writer out(target);
        out.add_data("abc", 3);
        CHECK(file_exists(target + ".tmp"));
    }
    CHECK(!file_exists(target + ".tmp"));
    CHECK(read_file(target) == std::vector<uint8_t>({'O', 'R', 'I', 'G', 'I', 'N', 'A', 'L'}));

    /* Invalid root: refused, target untouched */
    CHECK(contains(error_of([&] {
        ogawa::Writer out(target);
        out.commit(out.add_data("abc", 3));
    }), "invalid root"));
    CHECK(read_file(target).size() == 8 && !file_exists(target + ".tmp"));

    /* Parent written before its child: refused */
    CHECK(contains(error_of([&] {
        ogawa::Writer out(target);
        out.add_group({1u << 20});
    }), "after its parent"));

    /* Successful commit replaces the existing file */
    CHECK(error_of([&] {
        ogawa::Writer out(target);
        uint64_t d = out.add_data("payload", 7);
        out.commit(out.add_group({d, ogawa::EMPTY_GROUP}));
    }) == "");
    ogawa::Stats s;
    CHECK(error_of([&] { ogawa::Reader in(target); s = ogawa::scan(in); }) == "");
    CHECK(s.groups == 1 && s.datas == 1 && s.dataBytes == 7);
}

/* ------------------------------------------------------------------------- */
/*  Fuzzing                                                                  */
/* ------------------------------------------------------------------------- */

static void test_fuzz(int scale) {
    std::cout << "ogawa: fuzzing (mutations + truncations)" << std::endl;
    pcg32 rng;
    rng.seed(0x5eed, 0x0ca5);
    size_t runs = 0, accepted = 0, rejected = 0, unexpected = 0;
    auto t0 = std::chrono::steady_clock::now();

    for (const char *name : REFERENCE_FILES) {
        const std::vector<uint8_t> original = read_file(data_path(name));
        const bool big = original.size() > 100000;
        const int mutations = (big ? 30 : 300) * scale, truncations = (big ? 12 : 40) * scale;
        const std::string path = temp_path(std::string("fuzz_") + name);

        auto run = [&](const std::vector<uint8_t> &bytes) {
            write_file(path, bytes);
            ++runs;
            try {
                ogawa::Reader in(path);
                ogawa::scan(in);
                ++accepted;
            } catch (const std::runtime_error &) {
                ++rejected;
            } catch (...) {
                ++unexpected;   /* bad_alloc, logic_error, ... */
            }
        };

        for (int i = 0; i < mutations; ++i) {
            std::vector<uint8_t> bytes = original;
            const uint32_t flips = 1 + rng.nextUInt(8);
            for (uint32_t k = 0; k < flips; ++k) {
                /* Bias half of the hits towards the header and group tables */
                const uint32_t range = (rng.nextUInt(2) == 0) ? std::min<uint32_t>(512, (uint32_t) bytes.size())
                                                              : (uint32_t) bytes.size();
                const uint32_t at = rng.nextUInt(range);
                switch (rng.nextUInt(3)) {
                    case 0: bytes[at] ^= (uint8_t) (1 << rng.nextUInt(8)); break;
                    case 1: bytes[at] = (uint8_t) rng.nextUInt(256); break;
                    default: bytes[at] = rng.nextUInt(2) ? 0xff : 0x00; break;
                }
            }
            run(bytes);
        }
        for (int i = 0; i < truncations; ++i) {
            const size_t len = rng.nextUInt((uint32_t) original.size());
            run(std::vector<uint8_t>(original.begin(), original.begin() + len));
        }
    }
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    std::cout << "  " << runs << " corrupted files: " << accepted << " still valid, "
              << rejected << " rejected, " << unexpected << " unexpected errors ("
              << (int) (s * 1000) << " ms)" << std::endl;
    CHECK(unexpected == 0);
    CHECK(accepted + rejected == runs);
}

void test_ogawa(int fuzz_scale) {
    test_crafted();
    test_reference_files();
    test_writer_safety();
    test_fuzz(fuzz_scale);
}
