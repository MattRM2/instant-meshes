/*
    abc_dump.cpp -- Developer tool: prints the object/property tree of an
    Alembic file as seen by src/abc.h (types, sample counts, metadata and
    small values). Target abc_dump, built with the unit tests.

    Usage: abc_dump file.abc            print the tree
           abc_dump --verify file.abc   recompute and check every hash
*/

#include "abc.h"

static const char *POD_NAMES[] = {
    "bool", "uint8", "int8", "uint16", "int16", "uint32", "int32", "uint64",
    "int64", "float16", "float32", "float64", "string", "wstring"
};

static std::string meta_string(const abc::MetaData &meta) {
    std::string s;
    for (const auto &kv : meta)
        s += (s.empty() ? "" : ";") + kv.first + "=" + kv.second;
    return s;
}

static void dump_properties(abc::Archive &ar, const abc::Property &compound, int indent) {
    for (const abc::Property &p : ar.properties(compound)) {
        std::cout << std::string(indent, ' ') << p.name;
        if (p.type == abc::Property::Compound) {
            std::cout << " {compound}";
        } else {
            std::cout << " [" << (p.type == abc::Property::Scalar ? "scalar " : "array ")
                      << POD_NAMES[p.pod] << " x" << p.extent << ", " << p.samples << " samples";
            std::vector<uint8_t> raw = ar.sample(p);
            std::cout << ", " << raw.size() << " bytes]";
            if (p.pod == abc::PodString) {
                std::cout << " = \"" << ar.sample_string(p) << "\"";
            } else if (p.pod != abc::PodFloat16 && p.pod != abc::PodWstring &&
                       p.pod != abc::PodBool && raw.size() <= 128) {
                std::vector<double> v = ar.sample_doubles(p);
                std::cout << " =";
                for (double x : v)
                    std::cout << " " << x;
            } else if (p.pod == abc::PodBool && raw.size() <= 16) {
                std::cout << " =";
                for (uint8_t b : raw)
                    std::cout << " " << (int) b;
            }
        }
        if (!p.meta.empty())
            std::cout << "  <" << meta_string(p.meta) << ">";
        std::cout << std::endl;
        if (p.type == abc::Property::Compound)
            dump_properties(ar, p, indent + 4);
    }
}

static void dump_object(abc::Archive &ar, const abc::Object &o, int indent) {
    std::cout << std::string(indent, ' ') << "OBJECT " << o.path;
    if (!o.meta.empty())
        std::cout << "  <" << meta_string(o.meta) << ">";
    std::cout << std::endl;
    dump_properties(ar, ar.properties(o), indent + 4);
    for (const abc::Object &child : ar.children(o))
        dump_object(ar, child, indent + 2);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "Usage: abc_dump [--verify] file.abc" << std::endl;
        return 2;
    }
    try {
        if (std::string(argv[1]) == "--verify" && argc > 2) {
            std::cout << "OK: " << abc::verify_hashes(argv[2]) << " objects, all hashes match" << std::endl;
            return 0;
        }
        abc::Archive ar(argv[1]);
        dump_object(ar, ar.top(), 0);
    } catch (const std::exception &e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
